/*
 * latency_probe.c -- Cadence real-time proof: per-token latency probe.
 *
 * A minimal llama.cpp client: it loads the local GGUF model, generates a
 * long answer and timestamps EVERY token, so the observed inter-token
 * latency can be reduced to min / mean / max / p99.
 *
 * The absolute CLOCK_MONOTONIC timestamp of each token is exported, which
 * allows the host report to correlate the token stream with the control
 * task window (both live in the same QNX guest, same clock).
 *
 * QNX 8.0 / x86_64.  Build: qcc -o latency_probe latency_probe.c <llama libs>
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "llama.h"

#include "rt_common.h"

struct probe_cfg {
    const char *model;
    const char *prompt;
    const char *out_path;
    const char *tag;
    int   n_predict;
    int   n_threads;
    int   n_ctx;
    int   ignore_eog;
};

static void batch_set_tokens(struct llama_batch_ext *batch, const llama_token *tokens,
                             int32_t n_tokens, llama_pos pos_0)
{
    llama_batch_ext_clear(batch);
    for (int32_t i = 0; i < n_tokens; ++i) {
        const int32_t idx = llama_batch_ext_add_token(batch, 0, tokens[i]);
        const llama_pos pos = pos_0 + i;

        llama_batch_ext_set_pos(batch, idx, &pos);
    }
    llama_batch_ext_set_output_logits(batch, n_tokens - 1, true);
}

static void usage(const char *p)
{
    printf("usage: %s -m model.gguf [-p prompt] [-n n_predict] [-t threads]\n"
           "          [-c n_ctx] [-t tag] [-o out.json] [--ignore-eog]\n", p);
}

static int parse_args(int argc, char **argv, struct probe_cfg *cfg)
{
    int i;

    cfg->model     = NULL;
    cfg->prompt    = "Ecris un long texte en francais sur les systemes temps reel embarques.";
    cfg->out_path  = NULL;
    cfg->tag       = "llm";
    cfg->n_predict = 256;
    cfg->n_threads = 4;
    cfg->n_ctx     = 2048;
    cfg->ignore_eog = 0;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            cfg->model = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            cfg->prompt = argv[++i];
        } else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
            cfg->n_predict = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-t") == 0 && i + 1 < argc) {
            cfg->n_threads = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            cfg->n_ctx = atoi(argv[++i]);
        } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            cfg->out_path = argv[++i];
        } else if (strcmp(argv[i], "--ignore-eog") == 0) {
            cfg->ignore_eog = 1;
        } else {
            usage(argv[0]);
            return -1;
        }
    }
    if (cfg->model == NULL) {
        usage(argv[0]);
        return -1;
    }
    return 0;
}

static void emit_stats(rt_json *j, const char *key, const rt_stats *s, double scale)
{
    rt_json_obj_open(j, key);
    rt_json_u64(j, "count", s->count);
    rt_json_num(j, "min", (double)s->min / scale, 3);
    rt_json_num(j, "mean", s->mean / scale, 3);
    rt_json_num(j, "max", (double)s->max / scale, 3);
    rt_json_num(j, "p50", (double)s->p50 / scale, 3);
    rt_json_num(j, "p95", (double)s->p95 / scale, 3);
    rt_json_num(j, "p99", (double)s->p99 / scale, 3);
    rt_json_num(j, "stddev", s->stddev / scale, 3);
    rt_json_obj_close(j);
}

static void emit_report(const struct probe_cfg *cfg, const rt_stats *st,
                         const uint64_t *lat, const uint64_t *ts,
                         size_t ntok, uint64_t t0, uint64_t t1,
                         uint64_t load_ms, rt_json *j)
{
    size_t i;

    rt_json_obj_open(j, NULL);
    rt_json_str(j, "tool", "latency_probe");
    rt_json_str(j, "tag", cfg->tag);
    rt_json_str(j, "model", cfg->model);
    rt_json_u64(j, "n_predict", (uint64_t)cfg->n_predict);
    rt_json_u64(j, "n_threads", (uint64_t)cfg->n_threads);
    rt_json_u64(j, "tokens", (uint64_t)ntok);
    rt_json_num(j, "model_load_s", (double)load_ms / 1000.0, 3);
    rt_json_num(j, "generation_s", (double)(t1 - t0) / 1e9, 6);
    rt_json_num(j, "throughput_tps",
                (t1 > t0) ? (double)ntok / ((double)(t1 - t0) / 1e9) : 0.0, 3);
    rt_json_u64(j, "start_ns", t0);
    rt_json_u64(j, "end_ns", t1);
    emit_stats(j, "token_latency_us", st, 1000.0);
    emit_stats(j, "token_latency_ms", st, 1000000.0);
    rt_json_arr_open(j, "token_ts_ns");
    for (i = 0; i < ntok; i++) {
        rt_json_u64(j, NULL, ts[i]);
    }
    rt_json_arr_close(j);
    rt_json_arr_open(j, "token_latency_ns");
    for (i = 0; i < ntok; i++) {
        rt_json_u64(j, NULL, lat[i]);
    }
    rt_json_arr_close(j);
    rt_json_obj_close(j);
}

int main(int argc, char **argv)
{
    struct probe_cfg cfg;
    struct llama_model_params model_params;
    struct llama_context_params ctx_params;
    struct llama_model *model = NULL;
    struct llama_context *ctx   = NULL;
    struct llama_sampler *smpl  = NULL;
    struct llama_batch_ext *batch = NULL;
    const struct llama_vocab *vocab;
    llama_token *prompt_tokens = NULL;
    int          n_prompt = 0;
    llama_token  filler_token = 0;
    uint64_t    *lat = NULL, *ts = NULL;
    size_t       ntok = 0, cap = 0;
    uint64_t     t0 = 0, t1 = 0, load_ms = 0;
    rt_json      json;
    rt_stats     stats;
    uint64_t     load_start;

    if (parse_args(argc, argv, &cfg) != 0) {
        return 2;
    }

    llama_backend_init();

    load_start = rt_now_ns();
    model_params = llama_model_default_params();
    model_params.n_gpu_layers = 0;
    model = llama_model_load_from_file(cfg.model, model_params);
    if (model == NULL) {
        fprintf(stderr, "%s: unable to load model %s\n", argv[0], cfg.model);
        return 1;
    }
    load_ms = (rt_now_ns() - load_start) / 1000000ULL;
    fprintf(stderr, "[probe] model loaded in %llu ms\n", (unsigned long long)load_ms);

    vocab = llama_model_get_vocab(model);
    n_prompt = -llama_tokenize(vocab, cfg.prompt, strlen(cfg.prompt), NULL, 0, true, true);
    if (n_prompt <= 0) {
        fprintf(stderr, "%s: failed to tokenize prompt\n", argv[0]);
        return 1;
    }
    prompt_tokens = malloc(sizeof(llama_token) * (size_t)n_prompt);
    if (prompt_tokens == NULL) {
        return 1;
    }
    if (llama_tokenize(vocab, cfg.prompt, strlen(cfg.prompt), prompt_tokens,
                       (int32_t)n_prompt, true, true) < 0) {
        fprintf(stderr, "%s: failed to tokenize prompt\n", argv[0]);
        return 1;
    }

    /* A neutral token used when the model wants to stop: the generation must
     * stay long enough to load the machine. */
    {
        llama_token ft[4];

        if (llama_tokenize(vocab, ".", 1, ft, 4, false, false) > 0) {
            filler_token = ft[0];
        }
    }

    ctx_params = llama_context_default_params();
    ctx_params.n_ctx      = (uint32_t)cfg.n_ctx;
    ctx_params.n_batch    = 512;
    ctx_params.n_ubatch   = 512;
    ctx_params.n_threads  = cfg.n_threads;
    ctx_params.no_perf    = false;
    ctx = llama_init_from_model(model, ctx_params);
    if (ctx == NULL) {
        fprintf(stderr, "%s: failed to create context\n", argv[0]);
        return 1;
    }

    smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());
    batch = llama_batch_ext_init(ctx);

    cap  = (size_t)cfg.n_predict + 16;
    lat  = malloc(sizeof(uint64_t) * cap);
    ts   = malloc(sizeof(uint64_t) * cap);
    if (lat == NULL || ts == NULL) {
        return 1;
    }

    batch_set_tokens(batch, prompt_tokens, n_prompt, 0);
    /* The prompt is decoded by the first iteration of the loop below (the
     * batch already holds it), exactly like llama.cpp's examples/simple. */
    t0 = rt_now_ns();
    {
        int      n_pos   = n_prompt;
        int      n_batch = n_prompt;
        uint64_t last_ts = t0;

        while (ntok < (size_t)cfg.n_predict) {
            llama_token id;

            if (llama_process(ctx, LLAMA_PROCESS_TYPE_DECODE, batch) != 0) {
                fprintf(stderr, "%s: decode failed\n", argv[0]);
                break;
            }
            n_pos += n_batch;

            id = llama_sampler_sample(smpl, ctx, -1);
            {
                uint64_t now = rt_now_ns();

                if (ntok > 0) {
                    lat[ntok - 1] = now - last_ts;
                }
                ts[ntok] = now - t0;
                last_ts  = now;
            }

            if (llama_vocab_is_eog(vocab, id)) {
                /* Keep generating: substitute a neutral token so the load
                 * stays constant (the point is the cadence, not the prose). */
                id = filler_token;
            }
            {
                char buf[128];
                int  n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);

                if (n > 0) {
                    buf[n] = '\0';
                    fputs(buf, stderr);
                }
                ntok++;
            }
            batch_set_tokens(batch, &id, 1, n_pos);
            n_batch = 1;
        }
        t1 = rt_now_ns();
    }
    fputc('\n', stderr);
    fflush(stderr);

    rt_compute_stats(&stats, lat, ntok > 0 ? ntok - 1 : 0);

    if (rt_json_init(&json, 65536) != 0) {
        return 1;
    }
    emit_report(&cfg, &stats, lat, ts, ntok, t0, t1, load_ms, &json);
    printf("%s\n", json.buf);
    fflush(stdout);

    if (cfg.out_path != NULL) {
        FILE *f = fopen(cfg.out_path, "w");

        if (f != NULL) {
            fprintf(f, "%s\n", json.buf);
            fclose(f);
        }
    }
    rt_json_free(&json);
    llama_batch_ext_free(batch);
    llama_sampler_free(smpl);
    llama_free(ctx);
    llama_model_free(model);
    free(lat);
    free(ts);
    free(prompt_tokens);
    return 0;
}
