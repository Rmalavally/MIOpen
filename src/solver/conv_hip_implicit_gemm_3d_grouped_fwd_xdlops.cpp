/*******************************************************************************
 *
 * MIT License
 *
 * Copyright (c) 2023 Advanced Micro Devices, Inc.
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

#include <vector>
#include <cstdint>

#include <miopen/solver.hpp>
#include <miopen/generic_search.hpp>
#include <miopen/conv/data_invoke_params.hpp>
#include <miopen/solver/problem_description_interpreter.hpp>
#if MIOPEN_BACKEND_HIP && MIOPEN_USE_COMPOSABLEKERNEL
#include <ck/library/tensor_operation_instance/gpu/grouped_convolution_forward.hpp>
#endif
MIOPEN_DECLARE_ENV_VAR(MIOPEN_DEBUG_3D_CONV_IMPLICIT_GEMM_HIP_FWD_XDLOPS)

namespace miopen {
namespace solver {

#if MIOPEN_BACKEND_HIP && MIOPEN_USE_COMPOSABLEKERNEL
template <typename DataType>
using DeviceOpGFwd = ck::tensor_operation::device::DeviceGroupedConvFwdMultipleD<
    3,
    ck::tensor_layout::convolution::NDHWGC,
    ck::tensor_layout::convolution::KZYXGC,
    ck::Tuple<>,
    ck::tensor_layout::convolution::NDHWGK,
    DataType,
    DataType,
    ck::Tuple<>,
    DataType,
    ck::tensor_operation::element_wise::PassThrough,
    ck::tensor_operation::element_wise::PassThrough,
    ck::tensor_operation::element_wise::PassThrough>;

template <typename DataType>
using DeviceOpGFwdPtrs =
    ck::tensor_operation::device::instance::DeviceOperationInstanceFactory<DeviceOpGFwd<DataType>>;

namespace {

struct CKArgs
{
    CKArgs(const ProblemDescription& problem)
    {
        G  = ProblemInterpreter::GetGroupCountG(problem);
        N  = ProblemInterpreter::GetBatchN(problem);
        K1 = ProblemInterpreter::GetOutputChannelK(problem);
        C1 = ProblemInterpreter::GetInputChannelC(problem);
        C  = C1 / G; // Number of input Channel per group
        K  = K1 / G; // Number of output Channel per group
        Hi = ProblemInterpreter::GetInputHeightHi(problem);
        Wi = ProblemInterpreter::GetInputWidthWi(problem);
        Ho = ProblemInterpreter::GetOutputHeightHo(problem);
        Wo = ProblemInterpreter::GetOutputWidthWo(problem);
        Y  = ProblemInterpreter::GetFilterHeightY(problem);
        X  = ProblemInterpreter::GetFilterWidthX(problem);
        Di = ProblemInterpreter::GetInputDepthDi(problem);
        Do = ProblemInterpreter::GetOutputDepthDo(problem);
        Z  = ProblemInterpreter::GetFilterDepthZ(problem);

        input  = {G, N, C, Di, Hi, Wi};
        output = {G, N, K, Do, Ho, Wo};
        weight = {G, K, C, Z, Y, X};

        // strides from NHWGC to GNCHW laout
        //in_strides  = {C, Hi * Wi * G * C, 1, Wi * G * C, G * C};
        //out_strides = {K, Ho * Wo * G * K, 1, Wo * G * K, G * K};
        //wei_strides = {K * Y * X * C, Y * X * C, 1, X * C, C};
        //in_strides  = {N * Di * Hi * Wi * C, Di * Hi * Wi * C, 1, Hi * Wi * C, Wi * C, C};
        //out_strides = {N * Do * Ho * Wo * K, Do * Ho * Wo * K, 1, Ho * Wo * K, Wo * K, K};
        //wei_strides = {K * Z * Y * X * C, Z* Y * X * C, 1, Y * X * C, X * C, C};
        in_strides  = {C, Di * Hi * Wi * G * C, 1, Hi * Wi * G * C, Wi * G * C, G * C};
        out_strides = {K, Do * Ho * Wo * G * K, 1, Ho * Wo * G * K, Wo * G * K, G * K};
        wei_strides = {C, Z * Y * X * G * C, 1, Y * X * G * C, X * G * C, G * C};
        strides     = {ProblemInterpreter::GetAdjustedConvolutionStrideD(problem),
                    ProblemInterpreter::GetAdjustedConvolutionStrideH(problem),
                    ProblemInterpreter::GetAdjustedConvolutionStrideW(problem)};
        dilation    = {ProblemInterpreter::GetAdjustedConvolutionDilationD(problem),
                    ProblemInterpreter::GetAdjustedConvolutionDilationH(problem),
                    ProblemInterpreter::GetAdjustedConvolutionDilationW(problem)};
        lPadding    = {ProblemInterpreter::GetInputLeftPadD(problem),
                    ProblemInterpreter::GetInputLeftPadH(problem),
                    ProblemInterpreter::GetInputLeftPadW(problem)};
        rPadding    = {ProblemInterpreter::GetAdjustedInputRightPadD(problem),
                    ProblemInterpreter::GetAdjustedInputRightPadH(problem),
                    ProblemInterpreter::GetAdjustedInputRightPadW(problem)};
    }
    int G;
    int N;
    int K;
    int C;
    int C1;
    int K1;
    int Hi;
    int Wi;
    int Di;
    int Ho;
    int Wo;
    int Do;
    int Y;
    int X;
    int Z;
    std::array<ck::index_t, 6> input;
    std::array<ck::index_t, 6> in_strides;
    std::array<ck::index_t, 6> output;
    std::array<ck::index_t, 6> out_strides;
    std::array<ck::index_t, 6> weight;
    std::array<ck::index_t, 6> wei_strides;
    std::array<ck::index_t, 3> strides;
    std::array<ck::index_t, 3> dilation;
    std::array<ck::index_t, 3> lPadding;
    std::array<ck::index_t, 3> rPadding;
};
} // namespace

template <typename DataType>
void PerformanceConfigHipImplicitGemm3DGroupFwdXdlops::Init(const ProblemDescription& problem)
{
    const auto args      = CKArgs{problem};
    const auto conv_ptrs = DeviceOpGFwdPtrs<DataType>::GetInstances();
    assert(!conv_ptrs.empty());
    for(int i = 0; i < conv_ptrs.size(); i++)
    {
        auto argument_ptr = conv_ptrs[i]->MakeArgumentPointer(nullptr,
                                                              nullptr,
                                                              {},
                                                              nullptr,
                                                              args.input,
                                                              args.in_strides,
                                                              args.weight,
                                                              args.wei_strides,
                                                              {},
                                                              {},
                                                              args.output,
                                                              args.out_strides,
                                                              args.strides,
                                                              args.dilation,
                                                              args.lPadding,
                                                              args.rPadding,
                                                              {},
                                                              {},
                                                              {});
        if(conv_ptrs[i]->IsSupportedArgument(argument_ptr.get()))
        {
            valid_kernels.push_back(conv_ptrs[i]->GetTypeString());
        }
    }
    assert(!valid_kernels.empty());
    this->index     = 0;
    this->kernel_id = valid_kernels[0];
}

template <typename DataType>
bool PerformanceConfigHipImplicitGemm3DGroupFwdXdlops::CheckIsSupportCKArgs(
    const ProblemDescription& problem) const
{
    const auto args      = CKArgs{problem};
    const auto conv_ptrs = DeviceOpGFwdPtrs<DataType>::GetInstances();
    int i                = 0;
    for(; i < conv_ptrs.size(); i++)
    {
        if(conv_ptrs[i]->GetTypeString() == this->kernel_id)
        {
            break;
        }
    }
    if(i == valid_kernels.size())
    {
        return false;
    }
    auto argument_ptr = conv_ptrs[i]->MakeArgumentPointer(nullptr,
                                                          nullptr,
                                                          {},
                                                          nullptr,
                                                          args.input,
                                                          args.in_strides,
                                                          args.weight,
                                                          args.wei_strides,
                                                          {},
                                                          {},
                                                          args.output,
                                                          args.out_strides,
                                                          args.strides,
                                                          args.dilation,
                                                          args.lPadding,
                                                          args.rPadding,
                                                          {},
                                                          {},
                                                          {});
    return conv_ptrs[i]->IsSupportedArgument(argument_ptr.get());
}

template <typename DataType>
bool ConvHipImplicitGemm3DGroupFwdXdlops::CheckCKApplicability(
    const ProblemDescription& problem) const
{
    const auto conv_ptrs = DeviceOpGFwdPtrs<DataType>::GetInstances();
    assert(!conv_ptrs.empty());
    const auto args = CKArgs{problem};
    if(!std::all_of(args.strides.begin(), args.strides.end(), [&](auto x) { return x == 1; }))
        return false;
    for(int i = 0; i < conv_ptrs.size(); i++)
    {
        auto argument_ptr = conv_ptrs[i]->MakeArgumentPointer(nullptr,
                                                              nullptr,
                                                              {},
                                                              nullptr,
                                                              args.input,
                                                              args.in_strides,
                                                              args.weight,
                                                              args.wei_strides,
                                                              {},
                                                              {},
                                                              args.output,
                                                              args.out_strides,
                                                              args.strides,
                                                              args.dilation,
                                                              args.lPadding,
                                                              args.rPadding,
                                                              {},
                                                              {},
                                                              {});
        if(conv_ptrs[i]->IsSupportedArgument(argument_ptr.get()))
            return true;
    }
    std::cout<<"Boom!!! No qualified instances! Game Over!"<<std::endl;
    return false;
}

namespace {

template <typename DataType>
void RunCKSolution(const Handle& handle,
                   const AnyInvokeParams& primitive_parameters,
                   const ProblemDescription& problem,
                   const PerformanceConfigHipImplicitGemm3DGroupFwdXdlops& config)
{
    const auto args      = CKArgs{problem};
    const auto conv_ptrs = DeviceOpGFwdPtrs<DataType>::GetInstances();
    int i                = 0;
    for(; i < conv_ptrs.size(); i++)
    {
        if(conv_ptrs[i]->GetTypeString() == config.kernel_id)
        {
            break;
        }
    }
    assert(i != conv_ptrs.size());
    auto& conv_ptr      = conv_ptrs.at(i);
    auto& data_ctx      = primitive_parameters.CastTo<conv::DataInvokeParams>();
    const auto& tensors = data_ctx.tensors;

    auto argument_ptr = conv_ptr->MakeArgumentPointer(
        const_cast<void*>( // NOLINT (cppcoreguidelines-pro-type-const-cast)
            static_cast<const void*>(tensors.in)),
        const_cast<void*>( // NOLINT (cppcoreguidelines-pro-type-const-cast)
            static_cast<const void*>(tensors.w)),
        {},
        static_cast<void*>(tensors.out),
        args.input,
        args.in_strides,
        args.weight,
        args.wei_strides,
        {},
        {},
        args.output,
        args.out_strides,
        args.strides,
        args.dilation,
        args.lPadding,
        args.rPadding,
        {},
        {},
        {});
    auto invoker_ptr            = conv_ptr->MakeInvokerPointer();
    const auto enable_profiling = handle.IsProfilingEnabled();

    float elapsed_time =
        invoker_ptr->Run(argument_ptr.get(), {handle.GetStream(), enable_profiling});
    if(enable_profiling)
    {
        handle.ResetKernelTime();
        handle.AccumKernelTime(elapsed_time);
    }
}

} // namespace
#endif

void PerformanceConfigHipImplicitGemm3DGroupFwdXdlops::HeuristicInit(
    const ProblemDescription& problem)
{
#if !MIOPEN_BACKEND_HIP || !MIOPEN_USE_COMPOSABLEKERNEL
    std::ignore = problem;
#else
    switch(problem.conv_problem.GetInDataType())
    {
    case miopenHalf: Init<ck::half_t>(problem); break;
    case miopenFloat: Init<float>(problem); break;
    case miopenInt8: Init<int8_t>(problem); break;
    case miopenInt32:
    case miopenInt8x4:
    case miopenBFloat16:
    case miopenDouble: break;
    }
#endif
}

bool PerformanceConfigHipImplicitGemm3DGroupFwdXdlops::SetNextValue(const ProblemDescription& problem)
{
    if(valid_kernels.empty())
    {
        this->HeuristicInit(problem);
        assert(!valid_kernels.empty());
        return true;
    }
    if((index + 1) < valid_kernels.size())
    {
        ++index;
        this->kernel_id = this->valid_kernels[index];
        return true;
    }
    else
        return false;
}

bool PerformanceConfigHipImplicitGemm3DGroupFwdXdlops::IsValidValue() const
{
    return index < valid_kernels.size();
}

bool PerformanceConfigHipImplicitGemm3DGroupFwdXdlops::IsValid(
    const ProblemDescription& problem) const
{
#if !MIOPEN_BACKEND_HIP || !MIOPEN_USE_COMPOSABLEKERNEL
    std::ignore = problem;
    return false;
#else
    switch(problem.conv_problem.GetInDataType())
    {
    case miopenHalf: return CheckIsSupportCKArgs<ck::half_t>(problem);
    case miopenFloat: return CheckIsSupportCKArgs<float>(problem);
    case miopenInt8: return CheckIsSupportCKArgs<int8_t>(problem);
    case miopenInt32:
    case miopenInt8x4:
    case miopenBFloat16:
    case miopenDouble: break;
    }
    return false;
#endif
}

bool PerformanceConfigHipImplicitGemm3DGroupFwdXdlops::operator==(
    const PerformanceConfigHipImplicitGemm3DGroupFwdXdlops& other) const
{
    return this->kernel_id == other.kernel_id;
}

PerformanceConfigHipImplicitGemm3DGroupFwdXdlops
ConvHipImplicitGemm3DGroupFwdXdlops::GetDefaultPerformanceConfig(const ConvolutionContext&,
    const ProblemDescription& problem) const
{
    PerformanceConfigHipImplicitGemm3DGroupFwdXdlops pp;
    pp.HeuristicInit(problem);
    return pp;
}

bool ConvHipImplicitGemm3DGroupFwdXdlops::IsValidPerformanceConfig(
    const ConvolutionContext&,
    const ProblemDescription& problem,
    const PerformanceConfigHipImplicitGemm3DGroupFwdXdlops& config) const
{
    return config.IsValid(problem);
}

PerformanceConfigHipImplicitGemm3DGroupFwdXdlops
ConvHipImplicitGemm3DGroupFwdXdlops::Search(const ConvolutionContext& ctx,
                                          const ProblemDescription& problem,
                                          const AnyInvokeParams& invoke_ctx) const
{
    return GenericSearch(*this, ctx, problem, invoke_ctx);
}

bool ConvHipImplicitGemm3DGroupFwdXdlops::IsApplicable(const ConvolutionContext& ctx,
                                                     const ProblemDescription& problem) const
{
#if !MIOPEN_BACKEND_HIP || !MIOPEN_USE_COMPOSABLEKERNEL
    std::ignore = ctx;
    std::ignore = problem;
    return false;
#else
    if(miopen::IsDisabled(MIOPEN_DEBUG_3D_CONV_IMPLICIT_GEMM_HIP_FWD_XDLOPS{}))
        return false;
    if(miopen::IsEnabled(MIOPEN_DEBUG_CONVOLUTION_DETERMINISTIC{}))
        return false;
    if(problem.conv_problem.GetInDataType() != problem.conv_problem.GetWeightsDataType() ||
       problem.conv_problem.GetWeightsDataType() != problem.conv_problem.GetOutDataType() ||
       problem.conv_problem.GetInDataType() != problem.conv_problem.GetOutDataType())
        return false;
    if(!problem.direction.IsForward())
        return false;
    if(!problem.Is3d())
        return false;
    if(!problem.IsLayoutNHWC())
        return false;
    const std::string& arch = ctx.GetStream().GetDeviceName();
    if(!(arch == "gfx908" || arch == "gfx90a"))
        return false;
    switch(problem.conv_problem.GetInDataType())
    {
    case miopenHalf: return CheckCKApplicability<ck::half_t>(problem);
    case miopenFloat: return CheckCKApplicability<float>(problem);
    case miopenInt8: return CheckCKApplicability<int8_t>(problem);
    case miopenInt32:
    case miopenInt8x4:
    case miopenBFloat16:
    case miopenDouble: break;
    }
    return false;
#endif
}

ConvSolution ConvHipImplicitGemm3DGroupFwdXdlops::GetSolution(
    const ConvolutionContext& ctx,
    const ProblemDescription& problem,
    const PerformanceConfigHipImplicitGemm3DGroupFwdXdlops& config) const
{
    std::ignore = ctx;
#if !MIOPEN_BACKEND_HIP || !MIOPEN_USE_COMPOSABLEKERNEL
    std::ignore = problem;
    std::ignore = config;
    return {};
#else
    ConvSolution result;
    result.invoker_factory = [=](const std::vector<Kernel>& kernels) {
        std::ignore = kernels;
        return [=](const Handle& handle, const AnyInvokeParams& primitive_parameters) {
            switch(problem.conv_problem.GetInDataType())
            {
            case miopenHalf:
                RunCKSolution<ck::half_t>(handle, primitive_parameters, problem, config);
                break;
            case miopenFloat:
                RunCKSolution<float>(handle, primitive_parameters, problem, config);
                break;
            case miopenInt8:
                RunCKSolution<int8_t>(handle, primitive_parameters, problem, config);
                break;
            case miopenInt32:
            case miopenInt8x4:
            case miopenBFloat16:
            case miopenDouble: break;
            }
        };
    };
    return result;
#endif
}

} // namespace solver
} // namespace miopen