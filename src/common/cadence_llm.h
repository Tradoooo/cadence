/*
 * cadence_llm.h — C interface to the llama.cpp runtime used by cadenced.
 *
 * The implementation lives in src/daemon/cadence_llm.cpp because rendering
 * the Jinja chat template needs the C++ API of llama.cpp.
 */
#ifndef CADENCE_LLM_H
#define CADENCE_LLM_H

#include <stddef.h>
#include <stdint.h>

#include "llama.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cadence_llm cadence_llm_t;

/*
 * Callback invoked for every generated token.
 * Returns 0 to continue the generation, non-zero to stop it (cancel).
 */
typedef int (*cadence_llm_token_cb)(void * user_data,
                                    const char * text,
                                    int token_id,
                                    int n_decoded,
                                    double elapsed_ms);

/* Load the model and create the inference context. NULL on failure. */
cadence_llm_t * cadence_llm_load(const char * model_path,
                                 int n_ctx,
                                 int n_threads,
                                 char * err,
                                 size_t err_size);

/* Release the model, the context and the sampler. */
void cadence_llm_free(cadence_llm_t * llm);

/*
 * Render the user prompt with the chat template embedded in the GGUF.
 * Reasoning is disabled (enable_thinking=false) so the answer comes out
 * immediately instead of being drowned by a thinking block.
 * Returns 0 on success, -1 on failure (err is filled in).
 */
int cadence_llm_build_prompt(cadence_llm_t * llm,
                             const char * user_prompt,
                             char * out,
                             size_t out_size,
                             char * err,
                             size_t err_size);

/* Generate a completion, calling cb for each token. Returns the token count. */
int cadence_llm_generate(cadence_llm_t * llm,
                          const char * prompt,
                          int max_tokens,
                          uint32_t seed,
                          cadence_llm_token_cb cb,
                          void * user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CADENCE_LLM_H */
