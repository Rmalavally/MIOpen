/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2017 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 *******************************************************************************/
#include <cassert>
#include <algorithm>
#include <miopen/errors.hpp>
#include <miopen/tensor.hpp>
#include <miopen/tensor_ops.hpp>
#include <miopen/float_equal.hpp>
#include <miopen/visit_float.hpp>
#include <numeric>

#define MIO_TENSOROCL_DEBUG 0

namespace miopen {

// Free Tensor Functions
static void CreateBitmapAndGrid(unsigned int& bitmap,
                                std::vector<std::size_t>& a_lens,
                                std::vector<std::size_t>& c_lens,
                                int& num_wg,
                                int& work,
                                int d)
{
    for(int i = d; i >= 0; i--)
    {
        if(a_lens[i] != 1)
        {
            bitmap |= (1 << (a_lens.size() - (i + 1)));
            num_wg *= a_lens[i];
        }
        else
        {
            work *= c_lens[i];
        }
    }
}

static bool IsBitmapLeadingOnes(unsigned int& bitmap, int n_size, int first_not_one)
{
    bool leading_ones = true;

    for(int i = first_not_one; i >= 0; i--)
    {
        bool is_one = (bitmap & (1 << (n_size - 1 - i))) != 0u;
        leading_ones &= is_one;
    }
    return leading_ones;
}

void OpTensor3d(Handle& handle,
                miopenTensorOp_t tensorOp,
                const void* alpha0,
                const TensorDescriptor& aTensorDesc,
                ConstData_t ATensor,
                const void* alpha1,
                const TensorDescriptor& bTensorDesc,
                ConstData_t BTensor,
                const void* beta,
                const TensorDescriptor& cTensorDesc,
                Data_t CTensor,
                const size_t Aoffset,
                const size_t Boffset,
                const size_t Coffset)
{
    auto alens = aTensorDesc.GetLengths();
    auto blens = bTensorDesc.GetLengths();
    auto clens = cTensorDesc.GetLengths();

    auto astrides = aTensorDesc.GetStrides();
    auto bstrides = bTensorDesc.GetStrides();
    auto cstrides = cTensorDesc.GetStrides();

    auto bsize = blens.size();

    // first_not_one is incorrect if btensor size equal to 1
    auto first_not_one = std::find_if(blens.rbegin(), blens.rend(), [](int i) { return i != 1; });
    auto d             = std::distance(blens.begin(), first_not_one.base());

    // quick fix
    int num_wg = first_not_one != blens.rend() ? (*first_not_one == 0 ? 1 : *first_not_one) : 1;
    int work_per_wg = std::accumulate(clens.begin() + d, clens.end(), 1, std::multiplies<int>());

    unsigned int bitmap = 0;
    // update bitmap for first_not_one
    bitmap |= (1 << (bsize - d));

    // (d-2) is because distance starts from 1 and 0
    // also, we need to go past the "first_not_one" as that is already
    // accounted for in the bitmap
    CreateBitmapAndGrid(bitmap, blens, clens, num_wg, work_per_wg, (d - 2));

#if(MIO_TENSOROCL_DEBUG == 1)
    printf("bitmap: %u\n", bitmap);
    printf("work_per_wg: %d, num_wg: %d\n", work_per_wg, num_wg);
#endif

    int num_wg_orig = num_wg;
    int max_num_wg  = 4096;
    num_wg          = num_wg > max_num_wg ? max_num_wg : num_wg;

    size_t local_threads = 256;

    std::string network_config{};

    network_config = std::to_string(bTensorDesc.GetType()) + std::to_string(aTensorDesc.GetType()) +
                     std::to_string(tensorOp);

    visit_float(bTensorDesc.GetType(), [&](auto as_float) {

        auto miopen_alpha0 = as_float(*(static_cast<const float*>(alpha0)));
        auto miopen_alpha1 = as_float(*(static_cast<const float*>(alpha1)));
        auto miopen_beta   = as_float(*(static_cast<const float*>(beta)));

        if(clens[0] == 1 && blens[0] == 1 && alens[0] == 1 &&
           (blens[1] == clens[1] || blens[1] == 1) && blens[2] == clens[2])
        {

            network_config += std::to_string(clens[2]) + std::to_string(clens[1]) +
                              std::to_string(float_equal(miopen_beta, 0.0)) +
                              std::to_string(static_cast<int>(blens[1] == 1)) +
                              std::to_string(max_num_wg);

            auto&& kernels = handle.GetKernels("Op2dTensorLite", network_config);

            if(!kernels.empty())
            {
                auto kernel = kernels.front();

                kernel(ATensor,
                       int(astrides[1]), // a_cstride,
                       BTensor,
                       int(bstrides[1]), // b_cstride,
                       CTensor,
                       int(cstrides[1]), // c_cstride,
                       miopen_alpha0,
                       miopen_alpha1,
                       miopen_beta,
                       long(Aoffset),
                       long(Boffset),
                       long(Coffset),
                       int(clens[1]));

                return;
            }
        }
        else
        {

            network_config +=
                std::to_string(max_num_wg) + std::to_string(local_threads) + std::to_string(num_wg);

            auto&& kernels = handle.GetKernels("Op3dTensorGeneric", network_config);

            if(!kernels.empty())
            {
                auto kernel = kernels.front();

                kernel(ATensor,
                       int(astrides[0]), // a_nstride,
                       int(astrides[1]), // a_cstride,
                       BTensor,
                       int(blens[1]),    // b_c,
                       int(blens[2]),    // b_h,
                       int(bstrides[0]), // b_nstride,
                       int(bstrides[1]), // b_cstride,
                       CTensor,
                       int(clens[1]),    // c_c,
                       int(clens[2]),    // c_h,
                       int(cstrides[0]), // c_nstride,
                       int(cstrides[1]), // c_cstride,
                       miopen_alpha0,
                       miopen_alpha1,
                       miopen_beta,
                       bitmap,
                       work_per_wg,
                       long(Aoffset),
                       long(Boffset),
                       long(Coffset),
                       int(num_wg_orig));

                return;
            }
        }

        std::string parms = " -DMIOPEN_TYPE=" + GetDataType(bTensorDesc.GetType());

        if(aTensorDesc.GetType() == miopenFloat)
        {
            parms += " -DMIOPEN_USE_FP16=0";
            parms += " -DMIOPEN_USE_FP32=1";
        }
        else if(aTensorDesc.GetType() == miopenHalf)
        {
            parms += " -DMIOPEN_USE_FP16=1";
            parms += " -DMIOPEN_USE_FP32=0";
        }

        parms += " -DMIOPEN_TENSOR_OP=";
        switch(tensorOp)
        {
        case 0: parms += "miopenAdd"; break;
        case 1: parms += "miopenMul"; break;
        case 2: parms += "miopenMin"; break;
        case 3: parms += "miopenMax"; break;
        }
        std::string program_name = "MIOpenTensorKernels.cl";

        const std::vector<size_t> vld{local_threads, 1, 1};

        if(clens[0] == 1 && blens[0] == 1 && alens[0] == 1 &&
           (blens[1] == clens[1] || blens[1] == 1) && blens[2] == clens[2])
        {
            parms += " -DUSE_2D_TENSOR_LITE";

            // for naive tensor ops
            size_t RD_BLCK              = (clens[2] % 4 == 0) ? 4 : (clens[2] % 2 == 0) ? 2 : 1;
            const std::string data_type = GetDataType(bTensorDesc.GetType());
            const std::string READ_TYPE =
                (RD_BLCK == 1) ? data_type : data_type + std::to_string(RD_BLCK);

            size_t MAP_RD = clens[2] / RD_BLCK;
            parms += " -DRD_BLCK=" + std::to_string(RD_BLCK) + " -DMAP_RD=" +
                     std::to_string(MAP_RD) + " -DREAD_TYPE=" + READ_TYPE;

            if(!float_equal(miopen_beta, 0.0))
            {
                parms += " -DBETA";
            }

            if(blens[1] == 1)
            {
                parms += " -DBIAS";
            }

            num_wg = clens[1];
            num_wg = num_wg > max_num_wg ? max_num_wg : num_wg;
            parms += " -DMAX_NUM_WG=" + std::to_string(max_num_wg);

            const std::vector<size_t> vgd1{MAP_RD, static_cast<size_t>(num_wg), 1};

            handle.AddKernel(
                "Op2dTensorLite", network_config, program_name, "Op2dTensorLite", vld, vgd1, parms)(
                ATensor,
                int(astrides[1]), // a_cstride,
                BTensor,
                int(bstrides[1]), // b_cstride,
                CTensor,
                int(cstrides[1]), // c_cstride,
                miopen_alpha0,
                miopen_alpha1,
                miopen_beta,
                long(Aoffset),
                long(Boffset),
                long(Coffset),
                int(clens[1]));
        }
        else
        {
            // Special case for adding tensors in place
            size_t global_threads;
            global_threads = num_wg * local_threads;
            const std::vector<size_t> vgd{global_threads, 1, 1};

            parms += " -DUSE_3D_TENSOR_GENERIC";
            parms += " -DMAX_NUM_WG=" + std::to_string(max_num_wg);

            handle.AddKernel("Op3dTensorGeneric",
                             network_config,
                             program_name,
                             "Op3dTensorGeneric",
                             vld,
                             vgd,
                             parms)(ATensor,
                                    int(astrides[0]), // a_nstride,
                                    int(astrides[1]), // a_cstride,
                                    BTensor,
                                    int(blens[1]),    // b_c,
                                    int(blens[2]),    // b_h,
                                    int(bstrides[0]), // b_nstride,
                                    int(bstrides[1]), // b_cstride,
                                    CTensor,
                                    int(clens[1]),    // c_c,
                                    int(clens[2]),    // c_h,
                                    int(cstrides[0]), // c_nstride,
                                    int(cstrides[1]), // c_cstride,
                                    miopen_alpha0,
                                    miopen_alpha1,
                                    miopen_beta,
                                    bitmap,
                                    work_per_wg,
                                    long(Aoffset),
                                    long(Boffset),
                                    long(Coffset),
                                    int(num_wg_orig));
        }
    });
}

void OpTensor4d(Handle& handle,
                miopenTensorOp_t tensorOp,
                const void* alpha0,
                const TensorDescriptor& aTensorDesc,
                ConstData_t ATensor,
                const void* alpha1,
                const TensorDescriptor& bTensorDesc,
                ConstData_t BTensor,
                const void* beta,
                const TensorDescriptor& cTensorDesc,
                Data_t CTensor,
                const size_t Aoffset,
                const size_t Boffset,
                const size_t Coffset)
{
    auto blens = bTensorDesc.GetLengths();
    auto clens = cTensorDesc.GetLengths();
    auto dims  = clens.size();

    auto astrides = aTensorDesc.GetStrides();
    auto bstrides = bTensorDesc.GetStrides();
    auto bsize    = blens.size();
    auto cstrides = cTensorDesc.GetStrides();

    // first_not_one is incorrect if btensor size equal to 1
    auto first_not_one = std::find_if(blens.rbegin(), blens.rend(), [](int i) { return i != 1; });
    auto d             = std::distance(blens.begin(), first_not_one.base());

    // quick fix
    int num_wg = first_not_one != blens.rend() ? (*first_not_one == 0 ? 1 : *first_not_one) : 1;
    int work_per_wg = std::accumulate(clens.begin() + d, clens.end(), 1, std::multiplies<int>());

    unsigned int bitmap = 0;
    // update bitmap for first_not_one
    bitmap |= (1 << (bsize - d));

    // (d-2) is because distance starts from 1 and 0
    // also, we need to go past the "first_not_one" as that is already
    // accounted for in the bitmap
    CreateBitmapAndGrid(bitmap, blens, clens, num_wg, work_per_wg, (d - 2));

#if(MIO_TENSOROCL_DEBUG == 1)
    printf("bitmap: %u\n", bitmap);
    printf("work_per_wg: %d, num_wg: %d\n", work_per_wg, num_wg);
#endif

    // Forward Convolution Bias specialization
    // for fwd-bias, bitmap looks like <0, 1, 0, 0>
    // Is the no. of work-groups and the work for each wg balanced?
    auto fwd_conv_bias = bitmap == (1 << 2) ? 1 : 0;
    auto incr_wg       = 0;
    // This block gives off indexing for 5d tensors, skipping
    if(fwd_conv_bias == 1 && dims < 5 && num_wg < 640 && work_per_wg > 256 && clens[0] > 0)
    { // 640 workgroups of size 256 needed to completely fill the GPU

        work_per_wg /= clens[0]; // c_n;
        num_wg *= clens[0];      // c_n;
        incr_wg = 1;
    }

    int num_wg_orig = num_wg;
    int max_num_wg  = 4096;
    num_wg          = num_wg > max_num_wg ? max_num_wg : num_wg;

    size_t local_threads = 256;

    // Does the bitmap contain leading ones, i.e. 1,1,1,0 or 1,1,0,0
    // or 1,1,1,1 or 1,0,0,0
    bool leading_ones = IsBitmapLeadingOnes(bitmap, dims, (d - 2));
    if(leading_ones && work_per_wg < 64)
    {
        local_threads = 64;
    }

    std::string network_config{};

    network_config += GetDataType(bTensorDesc.GetType()) + std::to_string(max_num_wg);

    std::string program_name = "MIOpenTensorKernels.cl";

    const std::vector<size_t> vld{local_threads, 1, 1};

    // Special case for adding tensors in place
    size_t global_threads;
    global_threads =
        (static_cast<int>(leading_ones) == 1 && (d - 1) == 3) ? num_wg : num_wg * local_threads;
    global_threads = (global_threads < local_threads) ? local_threads : global_threads;

    const std::vector<size_t> vgd{global_threads, 1, 1};

    bool packed_tensor = true;

    // auto alens = aTensorDesc.GetLengths();
    packed_tensor &= aTensorDesc.IsPacked();
    packed_tensor &= bTensorDesc.IsPacked();
    packed_tensor &= cTensorDesc.IsPacked();

    bool packed_equal_tensor =
        packed_tensor && (bTensorDesc.GetElementSize() == cTensorDesc.GetElementSize());

#if(MIO_TENSOROCL_DEBUG == 1)
    printf("packed_tensor: %d\n", packed_tensor);
    printf("equal_tensor: %d\n", bTensorDesc.GetElementSize() == cTensorDesc.GetElementSize());
#endif

    network_config += std::to_string(bTensorDesc.GetType()) +
                      std::to_string(aTensorDesc.GetType()) + std::to_string(tensorOp) +
                      std::to_string(global_threads) + std::to_string(local_threads);

    visit_float(bTensorDesc.GetType(), [&](auto as_float) {

        auto miopen_alpha0 = as_float(*(static_cast<const float*>(alpha0)));
        auto miopen_alpha1 = as_float(*(static_cast<const float*>(alpha1)));
        auto miopen_beta   = as_float(*(static_cast<const float*>(beta)));

        if(fwd_conv_bias != 0)
        {
            network_config += std::to_string(incr_wg);

            if(packed_tensor)
            {
                auto&& kernels = handle.GetKernels("OpTensorFwdBias", network_config);

                if(!kernels.empty())
                {
                    auto kernel = kernels.front();
                    kernel(ATensor,
                           BTensor,
                           int(blens[1]),
                           CTensor,
                           int(clens[0]),
                           int(cstrides[0]),
                           int(cstrides[1]),
                           work_per_wg,
                           miopen_alpha0,
                           miopen_alpha1,
                           miopen_beta,
                           long(Aoffset),
                           long(Boffset),
                           long(Coffset),
                           int(num_wg_orig));

                    return;
                }
            }
            else
            {

                auto&& kernels = handle.GetKernels("OpTensorFwdBiasGeneric", network_config);

                if(!kernels.empty())
                {
                    auto kernel = kernels.front();
                    kernel(ATensor,
                           int(astrides[0]),
                           int(astrides[1]),
                           int(astrides[2]),
                           BTensor,
                           int(blens[1]),
                           int(bstrides[1]),
                           CTensor,
                           int(clens[0]),
                           int(clens[3]),
                           int(cstrides[0]),
                           int(cstrides[1]),
                           int(cstrides[2]),
                           miopen_alpha0,
                           miopen_alpha1,
                           miopen_beta,
                           work_per_wg,
                           long(Aoffset),
                           long(Boffset),
                           long(Coffset),
                           int(num_wg_orig));
                    return;
                }
            }
        }
        // precede leading_ones for bitmap = 1,1,1,1
        else if(packed_equal_tensor)
        {
            network_config += std::to_string(bTensorDesc.GetElementSize()) +
                              std::to_string(float_equal(miopen_beta, 0.0));
            auto&& kernels = handle.GetKernels("Op4dTensorLite", network_config);
            if(!kernels.empty())
            {
                auto kernel = kernels.front();
                kernel(ATensor,
                       BTensor,
                       CTensor,
                       miopen_alpha0,
                       miopen_alpha1,
                       miopen_beta,
                       long(Aoffset),
                       long(Boffset),
                       long(Coffset));
                return;
            }
        }
        else if(leading_ones)
        {
            network_config += std::to_string(d - 1);
            if(packed_tensor)
            {

                auto&& kernels = handle.GetKernels("OpTensorLeadingOnes", network_config);

                if(!kernels.empty())
                {
                    auto kernel = kernels.front();
                    kernel(ATensor,
                           BTensor,
                           CTensor,
                           int(clens[1]),
                           int(clens[2]),
                           int(clens[3]),
                           int(cstrides[0]),
                           int(cstrides[1]),
                           work_per_wg,
                           miopen_alpha0,
                           miopen_alpha1,
                           miopen_beta,
                           long(Aoffset),
                           long(Boffset),
                           long(Coffset),
                           int(num_wg_orig));

                    return;
                }
            }
            else
            {
                auto&& kernels = handle.GetKernels("OpTensorLeadingOnesGeneric", network_config);

                if(!kernels.empty())
                {
                    auto kernel = kernels.front();
                    kernel(ATensor,
                           int(astrides[0]),
                           int(astrides[1]),
                           int(astrides[2]),
                           BTensor,
                           int(bstrides[0]),
                           int(bstrides[1]),
                           int(bstrides[2]),
                           CTensor,
                           int(clens[1]),
                           int(clens[2]),
                           int(clens[3]),
                           int(cstrides[0]),
                           int(cstrides[1]),
                           int(cstrides[2]),
                           miopen_alpha0,
                           miopen_alpha1,
                           miopen_beta,
                           work_per_wg,
                           long(Aoffset),
                           long(Boffset),
                           long(Coffset),
                           int(num_wg_orig));
                    return;
                }
            }
        }
        else
        {
            auto&& kernels = handle.GetKernels("Op4dTensorGeneric", network_config);

            if(!kernels.empty())
            {
                auto kernel = kernels.front();
                kernel(ATensor,
                       int(astrides[0]), // a_nstride,
                       int(astrides[1]), // a_cstride,
                       int(astrides[2]), // a_hstride,
                       BTensor,
                       int(blens[1]),    // b_c,
                       int(blens[2]),    // b_h,
                       int(blens[3]),    // b_w,
                       int(bstrides[0]), // b_nstride,
                       int(bstrides[1]), // b_cstride,
                       int(bstrides[2]), // b_hstride,
                       CTensor,
                       int(clens[1]),    // c_c,
                       int(clens[2]),    // c_h,
                       int(clens[3]),    // c_w,
                       int(cstrides[0]), // c_nstride,
                       int(cstrides[1]), // c_cstride,
                       int(cstrides[2]), // c_hstride,
                       miopen_alpha0,
                       miopen_alpha1,
                       miopen_beta,
                       bitmap,
                       work_per_wg,
                       long(Aoffset),
                       long(Boffset),
                       long(Coffset),
                       int(num_wg_orig));
                return;
            }
        }

        std::string parms = " -DMIOPEN_TYPE=" + GetDataType(bTensorDesc.GetType()) +
                            " -DMAX_NUM_WG=" + std::to_string(max_num_wg);

        if(aTensorDesc.GetType() == miopenFloat)
        {
            parms += " -DMIOPEN_USE_FP16=0";
            parms += " -DMIOPEN_USE_FP32=1";
        }
        else if(aTensorDesc.GetType() == miopenHalf)
        {
            parms += " -DMIOPEN_USE_FP16=1";
            parms += " -DMIOPEN_USE_FP32=0";
        }

        parms += " -DMIOPEN_TENSOR_OP=";
        switch(tensorOp)
        {
        case 0: parms += "miopenAdd"; break;
        case 1: parms += "miopenMul"; break;
        case 2: parms += "miopenMin"; break;
        case 3: parms += "miopenMax"; break;
        }

        if(fwd_conv_bias != 0)
        {
            parms += " -DINCR_WG=" + std::to_string(incr_wg);

            if(packed_tensor)
            {
                parms += " -DUSE_FWD_BIAS";

                handle.AddKernel("OpTensorFwdBias",
                                 network_config,
                                 program_name,
                                 "OpTensorFwdBias",
                                 vld,
                                 vgd,
                                 parms)(ATensor,
                                        BTensor,
                                        int(blens[1]),
                                        CTensor,
                                        int(clens[0]),
                                        int(cstrides[0]),
                                        int(cstrides[1]),
                                        work_per_wg,
                                        miopen_alpha0,
                                        miopen_alpha1,
                                        miopen_beta,
                                        long(Aoffset),
                                        long(Boffset),
                                        long(Coffset),
                                        int(num_wg_orig));
            }
            else
            {
                parms += " -DUSE_FWD_BIAS_GENERIC";
                handle.AddKernel("OpTensorFwdBiasGeneric",
                                 network_config,
                                 program_name,
                                 "OpTensorFwdBiasGeneric",
                                 vld,
                                 vgd,
                                 parms)(ATensor,
                                        int(astrides[0]),
                                        int(astrides[1]),
                                        int(astrides[2]),
                                        BTensor,
                                        int(blens[1]),
                                        int(bstrides[1]),
                                        CTensor,
                                        int(clens[0]),
                                        int(clens[3]),
                                        int(cstrides[0]),
                                        int(cstrides[1]),
                                        int(cstrides[2]),
                                        miopen_alpha0,
                                        miopen_alpha1,
                                        miopen_beta,
                                        work_per_wg,
                                        long(Aoffset),
                                        long(Boffset),
                                        long(Coffset),
                                        int(num_wg_orig));
            }
        }
        // precede leading_ones for bitmap = 1,1,1,1
        else if(packed_equal_tensor)
        {
            parms += " -DUSE_4D_TENSOR_LITE";
            // for naive tensor ops
            size_t RD_BLCK              = (clens[2] % 4 == 0) ? 4 : (clens[2] % 2 == 0) ? 2 : 1;
            const std::string data_type = GetDataType(bTensorDesc.GetType());

            size_t MAP_RD   = clens[2] / RD_BLCK;
            size_t TENS_LEN = cTensorDesc.GetElementSize();
            RD_BLCK =
                (TENS_LEN % 4 == 0) ? 4 : (TENS_LEN % 3 == 0) ? 3 : (TENS_LEN % 2 == 0) ? 2 : 1;
            MAP_RD = TENS_LEN / RD_BLCK;

            const std::string READ_TYPE =
                (RD_BLCK == 1) ? data_type : data_type + std::to_string(RD_BLCK);

            parms += " -DRD_BLCK=" + std::to_string(RD_BLCK) + " -DMAP_RD=" +
                     std::to_string(MAP_RD) + " -DREAD_TYPE=" + READ_TYPE;

            if(!float_equal(miopen_beta, 0.0))
            {
                parms += " -DBETA";
            }

            const std::vector<size_t> vgd1{TENS_LEN / RD_BLCK, 1, 1};

            handle.AddKernel(
                "Op4dTensorLite", network_config, program_name, "Op4dTensorLite", vld, vgd1, parms)(
                ATensor,
                BTensor,
                CTensor,
                miopen_alpha0,
                miopen_alpha1,
                miopen_beta,
                long(Aoffset),
                long(Boffset),
                long(Coffset));
        }
        else if(leading_ones)
        {
            parms += " -DFIRST_NOT_ONE=" + std::to_string(d - 1);
            if(packed_tensor)
            {
                parms += " -DUSE_LEADING_ONES";
                handle.AddKernel("OpTensorLeadingOnes",
                                 network_config,
                                 program_name,
                                 "OpTensorLeadingOnes",
                                 vld,
                                 vgd,
                                 parms)(ATensor,
                                        BTensor,
                                        CTensor,
                                        int(clens[1]),
                                        int(clens[2]),
                                        int(clens[3]),
                                        int(cstrides[0]),
                                        int(cstrides[1]),
                                        work_per_wg,
                                        miopen_alpha0,
                                        miopen_alpha1,
                                        miopen_beta,
                                        long(Aoffset),
                                        long(Boffset),
                                        long(Coffset),
                                        int(num_wg_orig));
            }
            else
            {

                parms += " -DUSE_LEADING_ONES_GENERIC";

                handle.AddKernel("OpTensorLeadingOnesGeneric",
                                 network_config,
                                 program_name,
                                 "OpTensorLeadingOnesGeneric",
                                 vld,
                                 vgd,
                                 parms)(ATensor,
                                        int(astrides[0]),
                                        int(astrides[1]),
                                        int(astrides[2]),
                                        BTensor,
                                        int(bstrides[0]),
                                        int(bstrides[1]),
                                        int(bstrides[2]),
                                        CTensor,
                                        int(clens[1]),
                                        int(clens[2]),
                                        int(clens[3]),
                                        int(cstrides[0]),
                                        int(cstrides[1]),
                                        int(cstrides[2]),
                                        miopen_alpha0,
                                        miopen_alpha1,
                                        miopen_beta,
                                        work_per_wg,
                                        long(Aoffset),
                                        long(Boffset),
                                        long(Coffset),
                                        int(num_wg_orig));
            }
        }
        else
        {
            parms += " -DUSE_4D_TENSOR_GENERIC";

            handle.AddKernel("Op4dTensorGeneric",
                             network_config,
                             program_name,
                             "Op4dTensorGeneric",
                             vld,
                             vgd,
                             parms)(ATensor,
                                    int(astrides[0]), // a_nstride,
                                    int(astrides[1]), // a_cstride,
                                    int(astrides[2]), // a_hstride,
                                    BTensor,
                                    int(blens[1]),    // b_c,
                                    int(blens[2]),    // b_h,
                                    int(blens[3]),    // b_w,
                                    int(bstrides[0]), // b_nstride,
                                    int(bstrides[1]), // b_cstride,
                                    int(bstrides[2]), // b_hstride,
                                    CTensor,
                                    int(clens[1]),    // c_c,
                                    int(clens[2]),    // c_h,
                                    int(clens[3]),    // c_w,
                                    int(cstrides[0]), // c_nstride,
                                    int(cstrides[1]), // c_cstride,
                                    int(cstrides[2]), // c_hstride,
                                    miopen_alpha0,
                                    miopen_alpha1,
                                    miopen_beta,
                                    bitmap,
                                    work_per_wg,
                                    long(Aoffset),
                                    long(Boffset),
                                    long(Coffset),
                                    int(num_wg_orig));
        }
    });
}

void OpTensorOther(Handle& handle,
                   miopenTensorOp_t tensorOp,
                   const void* alpha0,
                   const TensorDescriptor& aTensorDesc,
                   ConstData_t ATensor,
                   const void* alpha1,
                   const TensorDescriptor& bTensorDesc,
                   ConstData_t BTensor,
                   const void* beta,
                   const TensorDescriptor& cTensorDesc,
                   Data_t CTensor,
                   const size_t Aoffset,
                   const size_t Boffset,
                   const size_t Coffset)
{
    auto blens = bTensorDesc.GetLengths();
    auto clens = cTensorDesc.GetLengths();

    auto astrides = aTensorDesc.GetStrides();
    auto bstrides = bTensorDesc.GetStrides();
    auto bsize    = blens.size();
    auto cstrides = cTensorDesc.GetStrides();

    // first_not_one is incorrect if btensor size equal to 1
    auto first_not_one = std::find_if(blens.rbegin(), blens.rend(), [](int i) { return i != 1; });
    auto d             = std::distance(blens.begin(), first_not_one.base());

    // quick fix
    int num_wg = first_not_one != blens.rend() ? (*first_not_one == 0 ? 1 : *first_not_one) : 1;
    int work_per_wg = std::accumulate(clens.begin() + d, clens.end(), 1, std::multiplies<int>());

    unsigned int bitmap = 0;
    // update bitmap for first_not_one
    bitmap |= (1 << (bsize - d));

    // (d-2) is because distance starts from 1 and 0
    // also, we need to go past the "first_not_one" as that is already
    // accounted for in the bitmap
    CreateBitmapAndGrid(bitmap, blens, clens, num_wg, work_per_wg, (d - 2));

#if(MIO_TENSOROCL_DEBUG == 1)
    printf("bitmap: %u\n", bitmap);
    printf("work_per_wg: %d, num_wg: %d\n", work_per_wg, num_wg);
#endif

    int num_wg_orig = num_wg;
    int max_num_wg  = 4096;
    num_wg          = num_wg > max_num_wg ? max_num_wg : num_wg;

    size_t local_threads = 256;

    std::string program_name = "MIOpenTensorKernels.cl";

    const std::vector<size_t> vld{local_threads, 1, 1};

    // Special case for adding tensors in place
    size_t global_threads;
    global_threads = num_wg * local_threads;

    const std::vector<size_t> vgd{global_threads, 1, 1};

    std::string network_config{};
    network_config += std::to_string(bTensorDesc.GetType()) +
                      std::to_string(aTensorDesc.GetType()) + std::to_string(tensorOp) +
                      std::to_string(global_threads) + std::to_string(local_threads);

    visit_float(bTensorDesc.GetType(), [&](auto as_float) {

        auto miopen_alpha0 = as_float(*(static_cast<const float*>(alpha0)));
        auto miopen_alpha1 = as_float(*(static_cast<const float*>(alpha1)));
        auto miopen_beta   = as_float(*(static_cast<const float*>(beta)));

        if(bsize == 5)
        {
            auto&& kernels = handle.GetKernels("Op5dTensorGeneric", network_config);

            if(!kernels.empty())
            {
                auto kernel = kernels.front();
                kernel(ATensor,
                       int(astrides[0]),
                       int(astrides[1]),
                       int(astrides[2]),
                       int(astrides[3]),
                       BTensor,
                       int(blens[1]),    // b_c,
                       int(blens[2]),    // b_d,
                       int(blens[3]),    // b_h,
                       int(blens[4]),    // b_w,
                       int(bstrides[0]), // b_nstride,
                       int(bstrides[1]), // b_cstride,
                       int(bstrides[2]), // b_dstride,
                       int(bstrides[3]), // b_hstride,
                       CTensor,
                       int(clens[1]),    // c_c,
                       int(clens[2]),    // c_d,
                       int(clens[3]),    // c_h,
                       int(clens[4]),    // c_w,
                       int(cstrides[0]), // c_nstride,
                       int(cstrides[1]), // c_cstride,
                       int(cstrides[2]), // c_dstride,
                       int(cstrides[3]), // c_hstride,
                       miopen_alpha0,
                       miopen_alpha1,
                       miopen_beta,
                       bitmap,
                       work_per_wg,
                       long(Aoffset),
                       long(Boffset),
                       long(Coffset),
                       int(num_wg_orig));
                return;
            }
        }
        else if(bsize == 2)
        {
            auto&& kernels = handle.GetKernels("Op2dTensorGeneric", network_config);

            if(!kernels.empty())
            {
                auto kernel = kernels.front();
                kernel(ATensor,
                       int(astrides[0]),
                       BTensor,
                       int(blens[1]),
                       int(bstrides[0]),
                       CTensor,
                       int(clens[1]),
                       int(cstrides[0]),
                       miopen_alpha0,
                       miopen_alpha1,
                       miopen_beta,
                       bitmap,
                       work_per_wg,
                       long(Aoffset),
                       long(Boffset),
                       long(Coffset),
                       int(num_wg_orig));
                return;
            }
        }
        else if(bsize == 1)
        {
            auto&& kernels = handle.GetKernels("Op1dTensorGeneric", network_config);

            if(!kernels.empty())
            {

                auto kernel = kernels.front();
                kernel(ATensor,
                       BTensor,
                       int(blens[0]),
                       CTensor,
                       int(clens[0]),
                       miopen_alpha0,
                       miopen_alpha1,
                       miopen_beta,
                       bitmap,
                       work_per_wg,
                       long(Aoffset),
                       long(Boffset),
                       long(Coffset),
                       int(num_wg_orig));
                return;
            }
        }

        std::string parms = " -DMIOPEN_TYPE=" + GetDataType(bTensorDesc.GetType()) +
                            " -DMAX_NUM_WG=" + std::to_string(max_num_wg);

        if(aTensorDesc.GetType() == miopenFloat)
        {
            parms += " -DMIOPEN_USE_FP16=0";
            parms += " -DMIOPEN_USE_FP32=1";
        }
        else if(aTensorDesc.GetType() == miopenHalf)
        {
            parms += " -DMIOPEN_USE_FP16=1";
            parms += " -DMIOPEN_USE_FP32=0";
        }

        parms += " -DMIOPEN_TENSOR_OP=";
        switch(tensorOp)
        {
        case 0: parms += "miopenAdd"; break;
        case 1: parms += "miopenMul"; break;
        case 2: parms += "miopenMin"; break;
        case 3: parms += "miopenMax"; break;
        }

        if(bsize == 5)
        {
            parms += " -DUSE_5D_TENSOR_GENERIC";

            handle.AddKernel("Op5dTensorGeneric",
                             network_config,
                             program_name,
                             "Op5dTensorGeneric",
                             vld,
                             vgd,
                             parms)(ATensor,
                                    int(astrides[0]),
                                    int(astrides[1]),
                                    int(astrides[2]),
                                    int(astrides[3]),
                                    BTensor,
                                    int(blens[1]),    // b_c,
                                    int(blens[2]),    // b_d,
                                    int(blens[3]),    // b_h,
                                    int(blens[4]),    // b_w,
                                    int(bstrides[0]), // b_nstride,
                                    int(bstrides[1]), // b_cstride,
                                    int(bstrides[2]), // b_dstride,
                                    int(bstrides[3]), // b_hstride,
                                    CTensor,
                                    int(clens[1]),    // c_c,
                                    int(clens[2]),    // c_d,
                                    int(clens[3]),    // c_h,
                                    int(clens[4]),    // c_w,
                                    int(cstrides[0]), // c_nstride,
                                    int(cstrides[1]), // c_cstride,
                                    int(cstrides[2]), // c_dstride,
                                    int(cstrides[3]), // c_hstride,
                                    miopen_alpha0,
                                    miopen_alpha1,
                                    miopen_beta,
                                    bitmap,
                                    work_per_wg,
                                    long(Aoffset),
                                    long(Boffset),
                                    long(Coffset),
                                    int(num_wg_orig));
        }
        else if(bsize == 2)
        {
            parms += " -DUSE_2D_TENSOR_GENERIC";

            handle.AddKernel("Op2dTensorGeneric",
                             network_config,
                             program_name,
                             "Op2dTensorGeneric",
                             vld,
                             vgd,
                             parms)(ATensor,
                                    int(astrides[0]),
                                    BTensor,
                                    int(blens[1]),
                                    int(bstrides[0]),
                                    CTensor,
                                    int(clens[1]),
                                    int(cstrides[0]),
                                    miopen_alpha0,
                                    miopen_alpha1,
                                    miopen_beta,
                                    bitmap,
                                    work_per_wg,
                                    long(Aoffset),
                                    long(Boffset),
                                    long(Coffset),
                                    int(num_wg_orig));
        }
        else if(bsize == 1)
        {
            parms += " -DUSE_1D_TENSOR_GENERIC";

            handle.AddKernel("Op1dTensorGeneric",
                             network_config,
                             program_name,
                             "Op1dTensorGeneric",
                             vld,
                             vgd,
                             parms)(ATensor,
                                    BTensor,
                                    int(blens[0]),
                                    CTensor,
                                    int(clens[0]),
                                    miopen_alpha0,
                                    miopen_alpha1,
                                    miopen_beta,
                                    bitmap,
                                    work_per_wg,
                                    long(Aoffset),
                                    long(Boffset),
                                    long(Coffset),
                                    int(num_wg_orig));
        }

    });
}

void OpTensor(Handle& handle,
              miopenTensorOp_t tensorOp,
              const void* alpha0,
              const TensorDescriptor& aTensorDesc,
              ConstData_t ATensor,
              const void* alpha1,
              const TensorDescriptor& bTensorDesc,
              ConstData_t BTensor,
              const void* beta,
              const TensorDescriptor& cTensorDesc,
              Data_t CTensor,
              const size_t Aoffset,
              const size_t Boffset,
              const size_t Coffset)
{
    if(ATensor == nullptr || BTensor == nullptr || CTensor == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }

    // if(aTensorDesc != cTensorDesc)
    if(aTensorDesc.GetElementSize() != cTensorDesc.GetElementSize())
    {
        MIOPEN_THROW("A and C Tensors do not match");
    }

    if(bTensorDesc.GetType() != cTensorDesc.GetType())
    {
        MIOPEN_THROW("Datatypes for B and C tensors do not match !");
    }

    auto blens = bTensorDesc.GetLengths();
#if(MIO_TENSOROCL_DEBUG == 1)
    printf("blen:[");
    for(auto len : blens)
    {
        printf(" %lu", len);
    }
    printf("]\n");
#endif
    auto clens = cTensorDesc.GetLengths();

    if(clens.size() > 5)
    {
        MIOPEN_THROW("Tensor dimension larger than 5: " + std::to_string(clens.size()));
    }

    if(blens.size() != clens.size())
    {
        MIOPEN_THROW("Number of dims in B and C Tensors do not match: " +
                     std::to_string(blens.size()) + ", " + std::to_string(clens.size()));
    }

    for(auto i = 0; i < clens.size(); i++)
    {
        if(blens[i] != 1 && blens[i] != clens[i])
        {
            MIOPEN_THROW("BTensor dim != 1 && BTensor dim != CTensor dim: " + std::to_string(i));
        }
    }

    auto bsize = blens.size();
    if(bsize == 3)
    {
        OpTensor3d(handle,
                   tensorOp,
                   alpha0,
                   aTensorDesc,
                   ATensor,
                   alpha1,
                   bTensorDesc,
                   BTensor,
                   beta,
                   cTensorDesc,
                   CTensor,
                   Aoffset,
                   Boffset,
                   Coffset);
    }
    else if(bsize == 4)
    {
        OpTensor4d(handle,
                   tensorOp,
                   alpha0,
                   aTensorDesc,
                   ATensor,
                   alpha1,
                   bTensorDesc,
                   BTensor,
                   beta,
                   cTensorDesc,
                   CTensor,
                   Aoffset,
                   Boffset,
                   Coffset);
    }
    else
    {
        OpTensorOther(handle,
                      tensorOp,
                      alpha0,
                      aTensorDesc,
                      ATensor,
                      alpha1,
                      bTensorDesc,
                      BTensor,
                      beta,
                      cTensorDesc,
                      CTensor,
                      Aoffset,
                      Boffset,
                      Coffset);
    }
}

static std::string parms_half_or_float(const miopenDataType_t t)
{
    std::string s{};

    switch(t)
    {
    case miopenHalf:
    {
        s = " -DMIOPEN_USE_FP16=1 -DMIOPEN_USE_FP32=0";
        break;
    }
    case miopenFloat:
    {
        s = " -DMIOPEN_USE_FP16=0 -DMIOPEN_USE_FP32=1";
        break;
    }
    }

    return s;
}

struct two_exp_ceiling_t
{
    std::size_t operator()(std::size_t n) const
    {
        assert(n > 0);

        std::size_t i = 1;

        n--;
        while(n != 0)
        {
            i *= 2;
            n /= 2;
        }

        return i;
    }
};

static std::vector<std::size_t> get_worker_sizes(const std::vector<std::size_t>& data_sizes)
{
    const std::size_t dim = data_sizes.size();

    std::vector<std::size_t> worker_sizes(dim);

    std::transform(data_sizes.begin(), data_sizes.end(), worker_sizes.begin(), two_exp_ceiling_t{});

    std::size_t wgd = std::accumulate(
        worker_sizes.begin(), worker_sizes.end(), std::size_t{1}, std::multiplies<std::size_t>());

    if(wgd > 65536)
    {
        std::size_t n = wgd / 65536;

        int i = 0;
        while(n > 1 && i < dim)
        {
            std::size_t size_old = worker_sizes[i];
            worker_sizes[i]      = (size_old - 1) / n + 1;
            n /= size_old / worker_sizes[i];
            ++i;
        }
    }

    return worker_sizes;
}

template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& vs)
{
    os << "{ ";
    for(auto& v : vs)
        os << v << " ";
    os << "}";
    return os;
}

template <typename T, std::size_t N>
std::ostream& operator<<(std::ostream& os, const std::array<T, N>& vs)
{
    os << "{ ";
    for(auto& v : vs)
        os << v << " ";
    os << "}";
    return os;
}

void flatten_tensor_descriptor(const TensorDescriptor& desc,
                                   std::size_t& flattened_dim,
                                   std::vector<std::size_t>& flattened_lengths,
                                   std::vector<std::size_t>& flattened_strides)
{
    flattened_dim = 0;
    flattened_lengths.clear();
    flattened_strides.clear();

    // is packed
    if(desc.IsPacked())
    {
        flattened_dim = 1;
        flattened_lengths.push_back(desc.GetElementSize());
        flattened_strides.push_back(1);

        return;
    }

    // is non-packed tensor, get rid of dimension, where length is 1
    std::size_t dim = 0;
    std::vector<std::size_t> lengths;
    std::vector<std::size_t> strides;

    for(std::size_t i = 0; i < desc.GetSize(); ++i)
    {
        std::size_t len = desc.GetLengths()[i];
        if(len > 1)
        {
            ++dim;
            lengths.push_back(len);
            strides.push_back(desc.GetStrides()[i]);
        }
    }

    std::cout << "get rid of 1 lengths: " << lengths << std::endl
              << "get rid of 1 strides: " << strides << std::endl;

    // is a scalar
    if(dim == 0)
    {
        flattened_dim = 1;
        flattened_lengths.push_back(1);
        flattened_strides.push_back(1);

        return;
    }

    // start flattening tensor
    std::vector<std::size_t> full_lengths(dim);

    full_lengths[0] = std::numeric_limits<std::size_t>::max();
    for(std::size_t i = 1; i < dim; ++i)
        full_lengths[i] = strides[i - 1] / strides[i];

    std::cout << __func__ << ": full_lengths: " << full_lengths << std::endl;

    auto flattened_len = lengths[0];
    for(std::size_t i = 1; i < dim; ++i)
    {
        auto len      = lengths[i];
        auto full_len = full_lengths[i];

        if(len == full_len)
            flattened_len *= len;
        else
        {
            flattened_lengths.push_back(flattened_len);
            flattened_strides.push_back(strides[i-1]);
            flattened_len = lengths[i];
        }
    }
    flattened_lengths.push_back(flattened_len);
    flattened_strides.push_back(strides[dim-1]);

    flattened_dim = flattened_lengths.size();

    std::cout << "flattened lengths: " << flattened_lengths << std::endl
              << "flattened strides: " << flattened_strides << std::endl;
    return;
}

void SetTensor(
    Handle& handle, const TensorDescriptor& yDesc, Data_t y, const void* alpha, const int offset)
{
    if(y == nullptr || alpha == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }

    std::size_t flattened_dim;
    std::vector<std::size_t> flattened_lengths;
    std::vector<std::size_t> flattened_strides;

    flatten_tensor_descriptor(
        yDesc, flattened_dim, flattened_lengths, flattened_strides);

    assert(flattened_dim > 0 && flattened_dim <= 5);

    std::string kernel_name = "SubTensorOpWithScalar" + std::to_string(flattened_dim) + "d";

    const miopenDataType_t dataType = yDesc.GetType();
    std::string network_config      = "set " + std::to_string(dataType);
    for(auto& len : flattened_lengths)
    {
        network_config += " " + std::to_string(len);
    }

    auto&& kernels = handle.GetKernels(kernel_name, network_config);

    KernelInvoke kernel;

    if(!kernels.empty())
    {
        kernel = kernels.front();
    }
    else
    {
        std::string program_name = "MIOpenSubTensorOpWithScalarKernel.cl";

        std::vector<std::size_t> worker_sizes = get_worker_sizes(flattened_lengths);

        std::size_t wgd = std::accumulate(worker_sizes.begin(),
                                          worker_sizes.end(),
                                          std::size_t{1},
                                          std::multiplies<std::size_t>());

        std::size_t wld = 256 < wgd ? 256 : wgd;

        std::string parms = "-DSUBTENSOR_OP_WITH_SCALAR=SUBTENSOR_OP_WITH_SCALAR_SET" +
                            parms_half_or_float(dataType);
        for(int i = 0; i < flattened_dim; ++i)
        {
            parms += " -DWORK_LENGTH_" + std::to_string(i) + "=" + std::to_string(worker_sizes[i]);
        }

        kernel = handle.AddKernel(kernel_name,
                                  network_config,
                                  program_name,
                                  kernel_name,
                                  {wld, 1, 1},
                                  {wgd, 1, 1},
                                  parms);
        std::cout << __func__ << std::endl
                  << "real lengths: " << yDesc.GetLengths() << std::endl
                  << "real strides: " << yDesc.GetStrides() << std::endl
                  << "flattened_lengths: " << flattened_lengths << std::endl
                  << "flattened_strides: " << flattened_strides << std::endl
                  << "worker_sizes: " << worker_sizes << std::endl
                  << "wgd: " << wgd << ", wld: " << wld << std::endl;
    }

    std::cout << __func__ << "global: " << kernel.global_work_dim << std::endl
              << "local: " << kernel.local_work_dim << std::endl
              << std::endl;
    switch(flattened_dim)
    {
    case 1:
    {
        visit_float((dataType), [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(flattened_strides[0]),
                   int(flattened_lengths[0]));
        });

        break;
    }
    case 2:
    {
        visit_float(dataType, [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(flattened_strides[0]),
                   int(flattened_strides[1]),
                   int(flattened_lengths[0]),
                   int(flattened_lengths[1]));
        });

        break;
    }
    case 3:
    {
        visit_float(dataType, [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(flattened_strides[0]),
                   int(flattened_strides[1]),
                   int(flattened_strides[2]),
                   int(flattened_lengths[0]),
                   int(flattened_lengths[1]),
                   int(flattened_lengths[2]));
        });

        break;
    }
    case 4:
    {
        visit_float(dataType, [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(flattened_strides[0]),
                   int(flattened_strides[1]),
                   int(flattened_strides[2]),
                   int(flattened_strides[3]),
                   int(flattened_lengths[0]),
                   int(flattened_lengths[1]),
                   int(flattened_lengths[2]),
                   int(flattened_lengths[3]));
        });

        break;
    }
    case 5:
    {
        visit_float(dataType, [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(flattened_strides[0]),
                   int(flattened_strides[1]),
                   int(flattened_strides[2]),
                   int(flattened_strides[3]),
                   int(flattened_strides[4]),
                   int(flattened_lengths[0]),
                   int(flattened_lengths[1]),
                   int(flattened_lengths[2]),
                   int(flattened_lengths[3]),
                   int(flattened_lengths[4]));
        });

        break;
    }
    default: assert(false);
    }
}

void ScaleTensor(
    Handle& handle, const TensorDescriptor& yDesc, Data_t y, const void* alpha, const int offset)
{
    if(y == nullptr || alpha == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm);
    }

    auto ydim = yDesc.GetLengths().size();

    assert(ydim > 0 && ydim <= 5);

    std::string kernel_name = "SubTensorOpWithScalar" + std::to_string(ydim) + "d";

    const std::vector<std::size_t>& lens = yDesc.GetLengths();

    std::string network_config = "scale " + std::to_string(yDesc.GetType());
    for(auto& len : lens)
    {
        network_config += " " + std::to_string(len);
    }

    auto&& kernels = handle.GetKernels(kernel_name, network_config);

    KernelInvoke kernel;

    if(!kernels.empty())
    {
        kernel = kernels.front();
    }
    else
    {
        std::string program_name = "MIOpenSubTensorOpWithScalarKernel.cl";

        std::vector<std::size_t> worker_sizes = get_worker_sizes(lens);

        std::size_t wgd = std::accumulate(worker_sizes.begin(),
                                          worker_sizes.end(),
                                          std::size_t{1},
                                          std::multiplies<std::size_t>());

        std::size_t wld = 256 < wgd ? 256 : wgd;

        std::string parms = "-DSUBTENSOR_OP_WITH_SCALAR=SUBTENSOR_OP_WITH_SCALAR_MULTIPLY" +
                            parms_half_or_float(yDesc.GetType());
        for(int i = 0; i < ydim; ++i)
        {
            parms += " -DWORK_LENGTH_" + std::to_string(i) + "=" + std::to_string(worker_sizes[i]);
        }

        kernel = handle.AddKernel(kernel_name,
                                  network_config,
                                  program_name,
                                  kernel_name,
                                  {wld, 1, 1},
                                  {wgd, 1, 1},
                                  parms);
    }

    switch(ydim)
    {
    case 1:
    {
        visit_float(yDesc.GetType(), [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(yDesc.GetStrides()[0]),
                   int(yDesc.GetLengths()[0]));
        });

        break;
    }
    case 2:
    {
        visit_float(yDesc.GetType(), [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(yDesc.GetStrides()[0]),
                   int(yDesc.GetStrides()[1]),
                   int(yDesc.GetLengths()[0]),
                   int(yDesc.GetLengths()[1]));
        });

        break;
    }
    case 3:
    {
        visit_float(yDesc.GetType(), [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(yDesc.GetStrides()[0]),
                   int(yDesc.GetStrides()[1]),
                   int(yDesc.GetStrides()[2]),
                   int(yDesc.GetLengths()[0]),
                   int(yDesc.GetLengths()[1]),
                   int(yDesc.GetLengths()[2]));
        });

        break;
    }
    case 4:
    {
        visit_float(yDesc.GetType(), [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(yDesc.GetStrides()[0]),
                   int(yDesc.GetStrides()[1]),
                   int(yDesc.GetStrides()[2]),
                   int(yDesc.GetStrides()[3]),
                   int(yDesc.GetLengths()[0]),
                   int(yDesc.GetLengths()[1]),
                   int(yDesc.GetLengths()[2]),
                   int(yDesc.GetLengths()[3]));
        });

        break;
    }
    case 5:
    {
        visit_float(yDesc.GetType(), [&](auto as_float) {
            kernel(y,
                   *as_float(alpha),
                   offset,
                   int(yDesc.GetStrides()[0]),
                   int(yDesc.GetStrides()[1]),
                   int(yDesc.GetStrides()[2]),
                   int(yDesc.GetStrides()[3]),
                   int(yDesc.GetStrides()[4]),
                   int(yDesc.GetLengths()[0]),
                   int(yDesc.GetLengths()[1]),
                   int(yDesc.GetLengths()[2]),
                   int(yDesc.GetLengths()[3]),
                   int(yDesc.GetLengths()[4]));
        });

        break;
    }
    default: assert(false);
    }
}

void CopyTensor(Handle& handle,
                const TensorDescriptor& srcDesc,
                ConstData_t src,
                const TensorDescriptor& dstDesc,
                Data_t dst,
                int srcOffset,
                int dstOffset)
{
    if(src == nullptr || dst == nullptr)
    {
        MIOPEN_THROW(miopenStatusBadParm, "Null pointer for tensor.");
    }
    if(srcDesc.GetElementSize() != dstDesc.GetElementSize())
    {
        MIOPEN_THROW(miopenStatusBadParm, "Tensor data sizes do not match.");
    }

    if(srcDesc.GetType() != dstDesc.GetType())
    {
        MIOPEN_THROW(miopenStatusBadParm, "Tensor types do not match.");
    }

    if(srcDesc.GetLengths().size() != dstDesc.GetLengths().size())
    {
        MIOPEN_THROW(miopenStatusBadParm, "Tensor dimension lengths do not match.");
    }

    if(srcDesc.GetLengths().size() > 5 || dstDesc.GetLengths().size() > 5)
    {
        MIOPEN_THROW(miopenStatusBadParm, "Tensor dimension sizes unsupported.");
    }

    if(srcOffset > 0 || dstOffset > 0 || srcDesc != dstDesc ||
       (srcDesc.GetElementSpace() != srcDesc.GetElementSize() ||
        dstDesc.GetElementSpace() != dstDesc.GetElementSize()))
    {
        auto srcDim = srcDesc.GetLengths().size();

        assert(srcDim > 0 && srcDim <= 5);

        std::string kernel_name = "SubTensorOpWithSubTensor" + std::to_string(srcDim) + "d";

        const std::vector<std::size_t>& lens = srcDesc.GetLengths();

        std::string network_config = "copy " + std::to_string(srcDesc.GetType());
        for(auto& len : lens)
        {
            network_config += " " + std::to_string(len);
        }

        auto&& kernels = handle.GetKernels(kernel_name, network_config);

        KernelInvoke kernel;

        if(!kernels.empty())
        {
            kernel = kernels.front();
        }
        else
        {
            std::string program_name = "MIOpenSubTensorOpWithSubTensorKernel.cl";

            std::vector<std::size_t> worker_sizes = get_worker_sizes(lens);

            std::size_t wgd = std::accumulate(worker_sizes.begin(),
                                              worker_sizes.end(),
                                              std::size_t{1},
                                              std::multiplies<std::size_t>());

            std::size_t wld = 256 < wgd ? 256 : wgd;

            std::string parms = "-DSUBTENSOR_OP_WITH_SUBTENSOR=SUBTENSOR_OP_WITH_SUBTENSOR_COPY" +
                                parms_half_or_float(srcDesc.GetType());
            for(int i = 0; i < srcDim; ++i)
            {
                parms +=
                    " -DWORK_LENGTH_" + std::to_string(i) + "=" + std::to_string(worker_sizes[i]);
            }

            kernel = handle.AddKernel(kernel_name,
                                      network_config,
                                      program_name,
                                      kernel_name,
                                      {wld, 1, 1},
                                      {wgd, 1, 1},
                                      parms);
        }

        switch(srcDim)
        {
        case 1:
        {
            kernel(src,
                   srcOffset,
                   int(srcDesc.GetStrides()[0]),
                   int(srcDesc.GetLengths()[0]),
                   dst,
                   dstOffset,
                   int(dstDesc.GetStrides()[0]));

            break;
        }
        case 2:
        {
            kernel(src,
                   srcOffset,
                   int(srcDesc.GetStrides()[0]),
                   int(srcDesc.GetStrides()[1]),
                   int(srcDesc.GetLengths()[0]),
                   int(srcDesc.GetLengths()[1]),
                   dst,
                   dstOffset,
                   int(dstDesc.GetStrides()[0]),
                   int(dstDesc.GetStrides()[1]));

            break;
        }
        case 3:
        {
            kernel(src,
                   srcOffset,
                   int(srcDesc.GetStrides()[0]),
                   int(srcDesc.GetStrides()[1]),
                   int(srcDesc.GetStrides()[2]),
                   int(srcDesc.GetLengths()[0]),
                   int(srcDesc.GetLengths()[1]),
                   int(srcDesc.GetLengths()[2]),
                   dst,
                   dstOffset,
                   int(dstDesc.GetStrides()[0]),
                   int(dstDesc.GetStrides()[1]),
                   int(dstDesc.GetStrides()[2]));

            break;
        }
        case 4:
        {
            kernel(src,
                   srcOffset,
                   int(srcDesc.GetStrides()[0]),
                   int(srcDesc.GetStrides()[1]),
                   int(srcDesc.GetStrides()[2]),
                   int(srcDesc.GetStrides()[3]),
                   int(srcDesc.GetLengths()[0]),
                   int(srcDesc.GetLengths()[1]),
                   int(srcDesc.GetLengths()[2]),
                   int(srcDesc.GetLengths()[3]),
                   dst,
                   dstOffset,
                   int(dstDesc.GetStrides()[0]),
                   int(dstDesc.GetStrides()[1]),
                   int(dstDesc.GetStrides()[2]),
                   int(dstDesc.GetStrides()[3]));

            break;
        }
        case 5:
        {
            kernel(src,
                   srcOffset,
                   int(srcDesc.GetStrides()[0]),
                   int(srcDesc.GetStrides()[1]),
                   int(srcDesc.GetStrides()[2]),
                   int(srcDesc.GetStrides()[3]),
                   int(srcDesc.GetStrides()[4]),
                   int(srcDesc.GetLengths()[0]),
                   int(srcDesc.GetLengths()[1]),
                   int(srcDesc.GetLengths()[2]),
                   int(srcDesc.GetLengths()[3]),
                   int(srcDesc.GetLengths()[4]),
                   dst,
                   dstOffset,
                   int(dstDesc.GetStrides()[0]),
                   int(dstDesc.GetStrides()[1]),
                   int(dstDesc.GetStrides()[2]),
                   int(dstDesc.GetStrides()[3]),
                   int(dstDesc.GetStrides()[4]));

            break;
        }
        default: assert(false);
        }
    }
    else
    {
        handle.Copy(src, dst, srcDesc.GetElementSize() * GetTypeSize(srcDesc.GetType()));
    }
}

} // namespace miopen
