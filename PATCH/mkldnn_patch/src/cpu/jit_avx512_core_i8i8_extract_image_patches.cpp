/*******************************************************************************
* Copyright 2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/
/*******************************************************************************
* Copyright 2017-2018 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#include <math.h>

#include "mkldnn_types.h"

#include "mkldnn_thread.hpp"
#include "utils.hpp"

#include "jit_generator.hpp"

#include "jit_avx512_core_i8i8_extract_image_patches.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

using namespace Xbyak;

using namespace mkldnn::impl::utils;
using namespace mkldnn::impl::memory_format;
using namespace mkldnn::impl::utils;
using namespace mkldnn::impl::types;

struct jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t: public jit_generator {
    DECLARE_CPU_JIT_AUX_FUNCTIONS(jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t)

    struct call_params_t {
        const char *src_i8;
        const char *dst_i8;
        size_t kw_range;
        size_t kh_range;
        float idivider;
    };

    Reg64 reg_ptr_src_i8 = r8;
    Reg64 reg_ptr_dst_i8 = r9;

    Reg64 ki = r10;
    Reg64 kj = r11;
    Reg64 reg_kw = r12;
    Reg64 reg_kh = r13;
    Reg64 c_iter = r14;

    Reg64 aux_reg_src_h = rax;
    Reg64 aux_reg_src_w = rbx;

    Reg64 reg_tmp = rdx;

    Reg64 reg_mask = r15;

    Opmask k_cmp_mask = Opmask(7);

    Opmask mask(int idx) {
        return Opmask(6 - idx);
    }

    Xmm xmm_tmp = Xmm(0);
    Zmm vreg_tmp = Zmm(30);
    Zmm vreg_zeros = Zmm(31);

    size_t sizeof_src_dt() const { return data_type_size(jpp.src_dt); }
    size_t sizeof_dst_dt() const { return data_type_size(jpp.dst_dt); }

    /* max extract_image_patches */
    Zmm vreg_src(int idx) {
        return Zmm(idx);
    }

    Zmm vreg_dst(int idx) {
        return Zmm(jpp.ur_c + idx);
    }

    /* avg extract_image_patches */
    Zmm vreg_src_s32(int jj, int ll) {
        return Zmm(12*jj + ll);
    }

    Zmm vreg_dst_s32(int jj, int ll) {
        return Zmm(12*jj + ll + 4);
    }

    Zmm vreg_dst_f32(int jj, int ll) {
        return Zmm(12*jj + ll + 8);
    }

    void (*ker_)(const call_params_t *);
    jit_extract_img_patches_conf_t jpp;

    void init_tmp_reg();
    void init_mask();

    void load_src(int jj, int ll, int c_tail);
    void store_dst(int jj, int ll, int c_tail);

    void compute_max_step(int ur_c, int c_tail);
    void compute_step(int ur_c, int c_tail);

    void compute_c_block();
    void generate();

    static status_t init_conf(jit_extract_img_patches_conf_t &jpp,
        const extract_image_patches_desc_t &pd, const memory_desc_wrapper &src_d,
        const memory_desc_wrapper &dst_d);

    jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t(const jit_extract_img_patches_conf_t &jpp_)
           : jpp(jpp_) {
        generate();
        ker_ = reinterpret_cast<decltype(ker_)>(const_cast<uint8_t*>(
                       getCode()));
    }
};

void jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::load_src(int jj, int ll, int c_tail) {
    using namespace data_type;

    int c_block = jpp.c_block;
    int ur_c = jpp.ur_c;

    auto offset = jj*c_block*sizeof_src_dt();
    if (jj == ur_c - 1 && c_tail) {
        if (jpp.src_dt == data_type::s32) {
            vmovups(vreg_src(jj) | mask(0),
                    ptr[aux_reg_src_w + offset]);
        } else {
            vmovdqu8(vreg_src(jj) | mask(0),
                    ptr[aux_reg_src_w + offset]);
        }
    } else {
        vmovups(vreg_src(jj), ptr[aux_reg_src_w + offset]);
    }
}

void jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::store_dst(int jj, int ll,
        int c_tail) {
    using namespace data_type;

    int c_block = jpp.c_block;
    int ur_c = jpp.ur_c;

    auto offset = jj*c_block*sizeof_dst_dt();
    if (jj == ur_c - 1 && c_tail) {
        if (jpp.src_dt == data_type::s32) {
            vmovups(ptr[reg_ptr_dst_i8 + offset],
                   vreg_dst(jj) | mask(0));
        } else {
            vmovdqu8(ptr[reg_ptr_dst_i8 + offset],
                    vreg_dst(jj) | mask(0));
        }
    } else {
        vmovups(ptr[reg_ptr_dst_i8 + offset], vreg_dst(jj));
    }
}

void jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::compute_max_step(int ur_c, int c_tail)
{
    Label l_kw, l_kh;

    int iw = jpp.iw;
    int c = jpp.c;

    for (int jj = 0; jj < ur_c; jj++)
        vmovups(vreg_dst(jj), vreg_tmp);

    mov(aux_reg_src_h, reg_ptr_src_i8);

    xor_(kj, kj);
    L(l_kh);
    {
        mov(aux_reg_src_w, aux_reg_src_h);
        xor_(ki, ki);
        L(l_kw);
        {
            for (int jj = 0; jj < ur_c; jj++) {
                load_src(jj, 0, c_tail);
                if (jpp.src_dt == data_type::s32) {
                    vpcmpd(k_cmp_mask, vreg_dst(jj), vreg_src(jj), _cmp_lt_os);
                    vpblendmd(vreg_dst(jj) | k_cmp_mask, vreg_dst(jj),
                            vreg_src(jj));
                } else {
                    if (jpp.src_dt == data_type::s8)
                        vpcmpb(k_cmp_mask, vreg_dst(jj), vreg_src(jj),
                                _cmp_lt_os);
                    else
                        vpcmpub(k_cmp_mask, vreg_dst(jj), vreg_src(jj),
                                _cmp_lt_os);
                    vpblendmb(vreg_dst(jj) | k_cmp_mask, vreg_dst(jj),
                            vreg_src(jj));
                }
            }
            add(aux_reg_src_w, c * sizeof_src_dt());
            inc(ki);
            cmp(ki, reg_kw);
            jl(l_kw, T_NEAR);
        }
        add(aux_reg_src_h, iw * c * sizeof_src_dt());
        inc(kj);
        cmp(kj, reg_kh);
        jl(l_kh, T_NEAR);
    }

    for (int jj = 0; jj < ur_c; jj++)
        store_dst(jj, 0, c_tail);
}

void jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::compute_step(int ur_c, int c_tail) {
    compute_max_step(ur_c, c_tail);
}

void jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::compute_c_block(){
    Label l_main_loop;

    int nb_c = jpp.nb_c;
    int c_block = jpp.c_block;
    int ur_c = jpp.ur_c;
    int ur_c_tail = jpp.ur_c_tail;
    int c_steps = nb_c / ur_c;
    int c_tail = jpp.c_tail;

    xor_(c_iter, c_iter);
    if (c_steps > 0) {
        L(l_main_loop); {
            compute_step(ur_c, 0);
            add(reg_ptr_src_i8, ur_c*c_block*sizeof_src_dt());
            add(reg_ptr_dst_i8, ur_c*c_block*sizeof_dst_dt());
            inc(c_iter);
            cmp(c_iter, c_steps);
            jl(l_main_loop, T_NEAR);
        }
    }

    if (ur_c_tail != 0) {
        compute_step(ur_c_tail, c_tail);
    }
}

void jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::init_mask() {
    for (int i = 0; i < 4; i++) {
        mov(reg_mask, jpp.tail[i]);
        kmovq(mask(i), reg_mask);
    }
}

void jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::init_tmp_reg() {
    using namespace data_type;

    switch (jpp.src_dt) {
        case s32:
            mov(reg_tmp, nstl::numeric_limits<int32_t>::lowest());
            break;
        case s8:
            mov(reg_tmp, nstl::numeric_limits<int8_t>::lowest());
            break;
        case u8:
            mov(reg_tmp, nstl::numeric_limits<uint8_t>::lowest());
            break;
        default: assert(!"unsupported src data_type");
    }

    movq(xmm_tmp, reg_tmp);
    if (jpp.src_dt == s32)
        vpbroadcastd(vreg_tmp, xmm_tmp);
    else
        vpbroadcastb(vreg_tmp, xmm_tmp);
}

void jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::generate() {
    preamble();

#   define READ_PARAM(reg, field) \
        mov(reg, ptr[abi_param1 + offsetof(call_params_t, field)])
    READ_PARAM(reg_ptr_src_i8, src_i8);
    READ_PARAM(reg_ptr_dst_i8, dst_i8);
    READ_PARAM(reg_kw, kw_range);
    READ_PARAM(reg_kh, kh_range);

#   undef READ_PARAM

    init_tmp_reg();
    init_mask();

    uni_vpxor(vreg_zeros, vreg_zeros, vreg_zeros);

    compute_c_block();

    postamble();
}

status_t jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::init_conf(jit_extract_img_patches_conf_t &jpp,
        const extract_image_patches_desc_t &pd, const memory_desc_wrapper &src_d,
        const memory_desc_wrapper &dst_d) {
    if (!mayiuse(avx512_core)) {
        return status::unimplemented;
    }

    jpp.mb = src_d.dims()[0];
    jpp.c = src_d.dims()[1];
    jpp.ih = src_d.dims()[2];
    jpp.iw = src_d.dims()[3];
    jpp.oh = dst_d.dims()[2];
    jpp.ow = dst_d.dims()[3];

    jpp.stride_h = pd.strides[0];
    jpp.stride_w = pd.strides[1];
    jpp.kh = pd.kernel[0];
    jpp.kw = pd.kernel[1];

    jpp.t_pad = pd.padding[0][0];
    jpp.l_pad = pd.padding[0][1];

    jpp.src_dt = pd.src_desc.data_type;
    jpp.dst_dt = pd.dst_desc.data_type;

    jpp.c_block = 64 / (jpp.src_dt == data_type::s32 ? 4 : 1);
    jpp.c_tail = jpp.c % jpp.c_block;
    jpp.nb_c = jpp.c / jpp.c_block;
    jpp.ur_c = 1;
    jpp.ur_c_tail = jpp.nb_c - (jpp.nb_c / jpp.ur_c)*jpp.ur_c +
            (jpp.c_tail != 0);

    size_t tail_mask = (1ULL << jpp.c_tail) - 1;

    jpp.tail[0] = tail_mask;
    jpp.tail[1] = 0;
    jpp.tail[2] = 0;
    jpp.tail[3] = 0;

    return status::success;
}

status_t jit_avx512_core_i8i8_extract_image_patches_fwd_t::pd_t::jit_conf() {
    return jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::init_conf(jpp_,
       desc_, src_pd_.desc(), dst_pd_.desc());
}

jit_avx512_core_i8i8_extract_image_patches_fwd_t::
jit_avx512_core_i8i8_extract_image_patches_fwd_t(const pd_t *pd,
          const input_vector &inputs, const output_vector &outputs)
    : cpu_primitive_t(&conf_, inputs, outputs), conf_(*pd), ker_(nullptr)
{ ker_ = new jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t(conf_.jpp_); }

jit_avx512_core_i8i8_extract_image_patches_fwd_t::
~jit_avx512_core_i8i8_extract_image_patches_fwd_t() { delete ker_; }

void jit_avx512_core_i8i8_extract_image_patches_fwd_t::execute_forward() {
    printf("src/cpu/jit_avx512_core_i8i8_extract_image_patches.cpp - execute_forward\n");
    auto src_i8 = reinterpret_cast<const char *>(input_memory(0));
    auto dst_i8 = reinterpret_cast<char *>(memory());

    const memory_desc_wrapper src_d(conf_.src_pd());
    const memory_desc_wrapper dst_d(conf_.dst_pd());

    const auto &jpp = conf_.jpp_;

    auto ker = [&](int ithr, int nthr) {
        const int work_amount = jpp.mb * jpp.oh * jpp.ow;

        int start{0}, end{0};
        balance211(work_amount, nthr, ithr, start, end);

        int n{0}, oh{0}, ow{0};
        nd_iterator_init(start, n, jpp.mb, oh, jpp.oh, ow, jpp.ow);

        auto p = jit_avx512_core_i8i8_extract_img_patches_fwd_ker_t::call_params_t();

        for (int iwork = start; iwork < end; ++iwork) {
            const int ih = nstl::max(oh*jpp.stride_h - jpp.t_pad, 0);
            const int iw = nstl::max(ow*jpp.stride_w - jpp.l_pad, 0);

            const int kh_start = nstl::max(0, jpp.t_pad - oh * jpp.stride_h);
            const int kh_end = nstl::min(jpp.kh,
                    jpp.ih + jpp.t_pad - oh * jpp.stride_h);
            const int kw_start = nstl::max(0, jpp.l_pad - ow * jpp.stride_w);
            const int kw_end = nstl::min(jpp.kw,
                    jpp.iw + jpp.l_pad - ow * jpp.stride_w);

            p.src_i8 = &src_i8[
                src_d.blk_off(n, 0, ih, iw) * src_d.data_type_size()];
            p.dst_i8 = &dst_i8[
                dst_d.blk_off(n, 0, oh, ow) * dst_d.data_type_size()];
            p.kw_range = (size_t)(kw_end - kw_start);
            p.kh_range = (size_t)(kh_end - kh_start);
            p.idivider = 1.0f / jpp.kw*jpp.kh;

            ker_->ker_(&p);

            nd_iterator_step(n, jpp.mb, oh, jpp.oh, ow, jpp.ow);
        }
    };

#   pragma omp parallel
    {
        ker(omp_get_thread_num(), omp_get_num_threads());
    }
}

}
}
}
