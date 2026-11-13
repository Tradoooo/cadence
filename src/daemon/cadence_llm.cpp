/*
 * cadence_llm.cpp — llama.cpp glue for the Cadence QNX service.
 *
 * Responsibilities:
 *  - load the GGUF model and create the inference context;
 *  - render the chat prompt with the Jinja template stored in the GGUF,
 *    with reasoning explicitly disabled (enable_thinking=false) so that the
 *    answer streams out immediately instead of being drowned by a thinking
 *    block;
 *  - run the generation loop and report every token through a callback.
 */
#include "cadence_llm.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cadence_msg.h"
#include "chat.h" /* common/chat.h : jinja chat templates */
#include "llama.h"

struct cadence_llm {
    llama_model *              model = nullptr;
    llama_context *            ctx = nullptr;
    const llama_vocab *        vocab = nullptr;
    common_chat_templates_ptr  tmpls;
    int                        n_ctx = 4096;
    int                        n_threads = 4;
};

static double now_ms(void)
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

/* Keep the llama.cpp traces out of the way, only warnings and errors. */
static void cadence_log_cb(ggml_log_level level, const char * text, void * user_data)
{
    (void) user_data;
    if (level >= GGML_LOG_LEVEL_WARN) {
        fputs(text, stderr);
        fflush(stderr);
    }
}

static void set_err(char * err, size_t err_size, const char * fmt, ...)
{
    if (!err || err_size == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, err_size, fmt, ap);
    va_end(ap);
}

extern "C" cadence_llm_t * cadence_llm_load(const char * model_path,
                                             int n_ctx,
                                             int n_threads,
                                             char * err,
                                             size_t err_size)
{
    llama_backend_init();
    llama_log_set(cadence_log_cb, NULL);

    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = 0; /* QNX guest: CPU only */

    llama_model * model = llama_model_load_from_file(model_path, mparams);
    if (!model) {
        set_err(err, err_size, "cannot load model '%s' (missing file or bad GGUF)", model_path);
        return nullptr;
    }

    llama_context_params cparams = llama_context_default_params();
    cparams.n_ctx           = n_ctx;
    cparams.n_batch         = 512;
    cparams.n_ubatch        = 512;
    cparams.n_threads       = n_threads;
    cparams.n_threads_batch = n_threads;

    llama_context * ctx = llama_init_from_model(model, cparams);
    if (!ctx) {
        llama_model_free(model);
        set_err(err, err_size, "cannot create the llama context (n_ctx=%d)", n_ctx);
        return nullptr;
    }

    cadence_llm_t * llm = new cadence_llm();
    llm->model     = model;
    llm->ctx       = ctx;
    llm->vocab     = llama_model_get_vocab(model);
    llm->n_ctx     = n_ctx;
    llm->n_threads = n_threads;
    llm->tmpls     = common_chat_templates_init(model, "");
    if (!llm->tmpls) {
        set_err(err, err_size, "no usable chat template in '%s'", model_path);
        cadence_llm_free(llm);
        return nullptr;
    }
    return llm;
}

extern "C" void cadence_llm_free(cadence_llm_t * llm)
{
    if (!llm) {
        return;
    }
    llm->tmpls.reset();
    if (llm->ctx) {
        llama_free(llm->ctx);
    }
    if (llm->model) {
        llama_model_free(llm->model);
    }
    delete llm;
}

extern "C" int cadence_llm_build_prompt(cadence_llm_t * llm,
                                         const char * user_prompt,
                                         char * out,
                                         size_t out_size,
                                         char * err,
                                         size_t err_size)
{
    if (!llm || !user_prompt || !out || out_size == 0) {
        set_err(err, err_size, "invalid arguments");
        return -1;
    }

    common_chat_msg msg;
    msg.role    = "user";
    msg.content = user_prompt;

    common_chat_templates_inputs inputs;
    inputs.messages                = { msg };
    inputs.add_generation_prompt   = true;
    inputs.use_jinja               = true;
    inputs.enable_thinking         = false;
    inputs.chat_template_kwargs["enable_thinking"] = "false";

    common_chat_params params = common_chat_templates_apply(llm->tmpls.get(), inputs);
    std::string prompt = params.prompt.str();

    if (prompt.empty()) {
        set_err(err, err_size, "chat template produced an empty prompt");
        return -1;
    }
    if (prompt.size() >= out_size) {
        set_err(err, err_size, "prompt too long (%zu bytes, max %zu)", prompt.size(), out_size - 1);
        return -1;
    }
    memcpy(out, prompt.c_str(), prompt.size() + 1);
    return 0;
}

extern "C" int cadence_llm_generate(cadence_llm_t * llm,
                                     const char * prompt,
                                     int max_tokens,
                                     uint32_t seed,
                                     cadence_llm_token_cb cb,
                                     void * user_data)
{
    if (!llm || !prompt) {
        return 0;
    }

    const int n_max = llm->n_ctx - 8;
    std::vector<llama_token> tokens(n_max);
    int n_prompt = llama_tokenize(llm->vocab, prompt, (int) strlen(prompt),
                                  tokens.data(), n_max, true, false);
    if (n_prompt < 0) {
        n_prompt = -n_prompt; /* tokenize() returns the negated size on overflow */
    }
    if (n_prompt <= 0 || n_prompt >= n_max) {
        return -1;
    }

    llama_memory_clear(llama_get_memory(llm->ctx), true);

    llama_batch batch = llama_batch_get_one(tokens.data(), n_prompt);
    if (llama_decode(llm->ctx, batch) != 0) {
        return -1;
    }

    llama_sampler_chain_params cparams = llama_sampler_chain_default_params();
    llama_sampler * smpl = llama_sampler_chain_init(cparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_top_k(40));
    llama_sampler_chain_add(smpl, llama_sampler_init_top_p(0.95f, 1));
    llama_sampler_chain_add(smpl, llama_sampler_init_temp(0.7f));
    llama_sampler_chain_add(smpl, llama_sampler_init_dist(seed ? seed : LLAMA_DEFAULT_SEED));

    const double t0 = now_ms();
    int n_decoded = 0;

    while (n_decoded < max_tokens) {
        llama_token token = llama_sampler_sample(smpl, llm->ctx, -1);
        if (llama_vocab_is_eog(llm->vocab, token)) {
            break;
        }
        llama_sampler_accept(smpl, token);

        char piece[CADENCE_MAX_TOKEN_TEXT];
        int  n_piece = llama_token_to_piece(llm->vocab, token, piece, (int) sizeof(piece), 0, false);
        if (n_piece < 0) {
            n_piece = 0;
        }
        piece[n_piece] = '\0';

        n_decoded++;
        if (cb && cb(user_data, piece, (int) token, n_decoded, now_ms() - t0) != 0) {
            break; /* cancelled by the client */
        }

        llama_token next[1] = { token };
        llama_batch b = llama_batch_get_one(next, 1);
        if (llama_decode(llm->ctx, b) != 0) {
            break;
        }
    }

    llama_sampler_free(smpl);
    return n_decoded;
}
