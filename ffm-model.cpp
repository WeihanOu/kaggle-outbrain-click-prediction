#include "ffm-model.h"

#include <iostream>
#include <iomanip>
#include <fstream>
#include <random>
#include <algorithm>

#include <immintrin.h>


// Define intrinsic missing in gcc
#define _mm256_set_m128(v0, v1)  _mm256_insertf128_ps(_mm256_castps128_ps256(v1), (v0), 1)


constexpr ffm_uint align_bytes = 32;
constexpr ffm_uint align_floats = align_bytes / sizeof(ffm_float);

constexpr ffm_ulong n_fields = 30;
constexpr ffm_ulong n_features = 1 << ffm_hash_bits;

constexpr ffm_ulong n_dim = 14;
constexpr ffm_ulong n_dim_aligned = ((n_dim - 1) / align_floats + 1) * align_floats;

constexpr ffm_ulong index_stride = n_fields * n_dim_aligned * 2;
constexpr ffm_ulong field_stride = n_dim_aligned * 2;

constexpr uint prefetch_depth = 1;


inline uint test_mask_bit(uint64_t * mask, uint i) {
    return (mask[i >> 6] >> (i & 63)) & 1;
}


inline void prefetch_interaction_weights(float * addr) {
    for (uint i = 0, sz = field_stride * sizeof(float); i < sz; i += 64)
        _mm_prefetch(((char *)addr) + i, _MM_HINT_T1);
}


inline ffm_float sum(__m256 val) {
    __m128 s = _mm256_extractf128_ps(_mm256_add_ps(val,  _mm256_permute2f128_ps(val, val, 1)), 0);

    s = _mm_hadd_ps(s, s);
    s = _mm_hadd_ps(s, s);

    float sum;
    _mm_store_ss(&sum, s);

    return sum;
}


static ffm_float * malloc_aligned_float(ffm_ulong size) {
    void *ptr;

    int status = posix_memalign(&ptr, align_bytes, size*sizeof(ffm_float));

    if(status != 0)
        throw std::bad_alloc();

    return (ffm_float*) ptr;
}


template <typename D>
static void init_weights(ffm_float * weights, ffm_uint n, D gen, std::default_random_engine & rnd) {
    ffm_float * w = weights;

    for(ffm_uint i = 0; i < n; i++) {
        for (ffm_uint d = 0; d < n_dim; d++, w++)
            *w = gen(rnd);

        for (ffm_uint d = n_dim; d < n_dim_aligned; d++, w++)
            *w = 0;

        for (ffm_uint d = n_dim_aligned; d < 2*n_dim_aligned; d++, w++)
            *w = 1;
    }
}


static void init_linear_weights(ffm_float * weights, ffm_uint n) {
    ffm_float * w = weights;

    for(ffm_uint i = 0; i < n; i++) {
        *w++ = 0;
        *w++ = 1;
    }
}

ffm_model::ffm_model(int seed, bool restricted, float eta, float lambda) {
    this->eta = eta;
    this->lambda = lambda;

    if (restricted) {
        max_b_field = 19;
        min_a_field = 10;
    } else {
        max_b_field = n_fields;
        min_a_field = 0;
    }

    std::default_random_engine rnd(seed);

    bias_w = 0;
    bias_wg = 1;

    weights = malloc_aligned_float(n_features * n_fields * n_dim_aligned * 2);
    linear_weights = malloc_aligned_float(n_features * 2);

    init_weights(weights, n_features * n_fields, std::uniform_real_distribution<ffm_float>(0.0, 1.0/sqrt(n_dim)), rnd);
    init_linear_weights(linear_weights, n_features);
}

ffm_model::~ffm_model() {
    free(weights);
    free(linear_weights);
}

ffm_float ffm_model::predict(const ffm_feature * start, const ffm_feature * end, ffm_float norm, uint64_t * mask) {
    ffm_float linear_total = 0;
    ffm_float linear_norm = end - start;

    __m256 xmm_total = _mm256_set1_ps(bias_w);

    ffm_uint i = 0;

    for (const ffm_feature * fa = start; fa != end; ++ fa) {
        ffm_uint index_a = fa->index &  ffm_hash_mask;
        ffm_uint field_a = fa->index >> ffm_hash_bits;
        ffm_float value_a = fa->value;

        linear_total += value_a * linear_weights[index_a*2] / linear_norm;

        if (field_a < min_a_field)
            continue;

        for (const ffm_feature * fb = start; fb != fa; ++ fb, ++ i) {
            ffm_uint index_b = fb->index &  ffm_hash_mask;
            ffm_uint field_b = fb->index >> ffm_hash_bits;
            ffm_float value_b = fb->value;

            if (field_b > max_b_field)
                break;

            if (fb + prefetch_depth < fa && test_mask_bit(mask, i + prefetch_depth)) { // Prefetch row only if no dropout
                ffm_uint index_p = fb[prefetch_depth].index &  ffm_hash_mask;
                ffm_uint field_p = fb[prefetch_depth].index >> ffm_hash_bits;

                prefetch_interaction_weights(weights + index_p * index_stride + field_a * field_stride);
                prefetch_interaction_weights(weights + index_a * index_stride + field_p * field_stride);
            }

            if (test_mask_bit(mask, i) == 0)
                continue;

            //if (field_a == field_b)
            //    continue;

            ffm_float * wa = weights + index_a * index_stride + field_b * field_stride;
            ffm_float * wb = weights + index_b * index_stride + field_a * field_stride;

            __m256 xmm_val = _mm256_set1_ps(value_a * value_b / norm);

            for(ffm_uint d = 0; d < n_dim; d += 8) {
                __m256 xmm_wa = _mm256_load_ps(wa + d);
                __m256 xmm_wb = _mm256_load_ps(wb + d);

                xmm_total = _mm256_add_ps(xmm_total, _mm256_mul_ps(_mm256_mul_ps(xmm_wa, xmm_wb), xmm_val));
            }
        }
    }

    return sum(xmm_total) + linear_total;
}


void ffm_model::update(const ffm_feature * start, const ffm_feature * end, ffm_float norm, ffm_float kappa, uint64_t * mask) {
    ffm_float linear_norm = end - start;

    __m256 xmm_eta = _mm256_set1_ps(eta);
    __m256 xmm_lambda = _mm256_set1_ps(lambda);

    ffm_uint i = 0;

    for (const ffm_feature * fa = start; fa != end; ++ fa) {
        ffm_uint index_a = fa->index &  ffm_hash_mask;
        ffm_uint field_a = fa->index >> ffm_hash_bits;
        ffm_float value_a = fa->value;

        ffm_float g = lambda * linear_weights[index_a*2] + kappa * value_a / linear_norm;
        ffm_float wg = linear_weights[index_a*2 + 1] + g*g;

        linear_weights[index_a*2] -= eta * g / sqrt(wg);
        linear_weights[index_a*2 + 1] = wg;

        if (field_a < min_a_field)
            continue;

        for (const ffm_feature * fb = start; fb != fa; ++ fb, ++ i) {
            ffm_uint index_b = fb->index &  ffm_hash_mask;
            ffm_uint field_b = fb->index >> ffm_hash_bits;
            ffm_float value_b = fb->value;

            if (field_b > max_b_field)
                break;

            if (fb + prefetch_depth < fa && test_mask_bit(mask, i + prefetch_depth)) { // Prefetch row only if no dropout
                ffm_uint index_p = fb[prefetch_depth].index &  ffm_hash_mask;
                ffm_uint field_p = fb[prefetch_depth].index >> ffm_hash_bits;

                prefetch_interaction_weights(weights + index_p * index_stride + field_a * field_stride);
                prefetch_interaction_weights(weights + index_a * index_stride + field_p * field_stride);
            }

            if (test_mask_bit(mask, i) == 0)
                continue;

            //if (field_a == field_b)
            //    continue;

            ffm_float * wa = weights + index_a * index_stride + field_b * field_stride;
            ffm_float * wb = weights + index_b * index_stride + field_a * field_stride;

            ffm_float * wga = wa + n_dim_aligned;
            ffm_float * wgb = wb + n_dim_aligned;

            __m256 xmm_kappa_val = _mm256_set1_ps(kappa * value_a * value_b / norm);

            for(ffm_uint d = 0; d < n_dim; d += 8) {
                // Load weights
                __m256 xmm_wa = _mm256_load_ps(wa + d);
                __m256 xmm_wb = _mm256_load_ps(wb + d);

                __m256 xmm_wga = _mm256_load_ps(wga + d);
                __m256 xmm_wgb = _mm256_load_ps(wgb + d);

                // Compute gradient values
                __m256 xmm_ga = _mm256_add_ps(_mm256_mul_ps(xmm_lambda, xmm_wa), _mm256_mul_ps(xmm_kappa_val, xmm_wb));
                __m256 xmm_gb = _mm256_add_ps(_mm256_mul_ps(xmm_lambda, xmm_wb), _mm256_mul_ps(xmm_kappa_val, xmm_wa));

                // Update weights
                xmm_wga = _mm256_add_ps(xmm_wga, _mm256_mul_ps(xmm_ga, xmm_ga));
                xmm_wgb = _mm256_add_ps(xmm_wgb, _mm256_mul_ps(xmm_gb, xmm_gb));

                xmm_wa  = _mm256_sub_ps(xmm_wa, _mm256_mul_ps(xmm_eta, _mm256_mul_ps(_mm256_rsqrt_ps(xmm_wga), xmm_ga)));
                xmm_wb  = _mm256_sub_ps(xmm_wb, _mm256_mul_ps(xmm_eta, _mm256_mul_ps(_mm256_rsqrt_ps(xmm_wgb), xmm_gb)));

                // Store weights
                _mm256_store_ps(wa + d, xmm_wa);
                _mm256_store_ps(wb + d, xmm_wb);

                _mm256_store_ps(wga + d, xmm_wga);
                _mm256_store_ps(wgb + d, xmm_wgb);
            }
        }
    }

    // Update bias
    bias_wg += kappa;
    bias_w -= eta * kappa / sqrt(bias_wg);
}