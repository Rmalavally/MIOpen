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

#include <miopen/groupnorm/solvers.hpp>

#include <miopen/groupnorm/invoke_params.hpp>
#include <miopen/datatype.hpp>
#include <miopen/groupnorm.hpp>
#include <miopen/kernel_build_params.hpp>
#include <miopen/target_properties.hpp>

#define LOCAL_SIZE 256

namespace miopen {

namespace solver {

namespace groupnorm {

std::size_t sizeof_kernel_FLOAT(const miopen::groupnorm::ProblemDescription& problem)
{
    const auto datatype = problem.GetXDesc().GetType();
    return get_data_size(datatype);
}

std::size_t sizeof_local_memory(const miopen::groupnorm::ProblemDescription& problem)
{
    std::size_t rv = 0;
    rv += LOCAL_SIZE * sizeof_kernel_FLOAT(problem) * 2;
    return rv;
}

bool GroupNormForward::IsApplicable(const ExecutionContext&,
                                    const miopen::groupnorm::ProblemDescription& problem) const
{
    if(!problem.IsSameType())
        return false;
    if(!problem.IsSameLength())
        return false;
    if(!problem.IsAllPacked())
        return false;
    if(!problem.IsRightNormDim())
        return false;
    if(!(sizeof_local_memory(problem) <= TargetProperties::GetMaxLocalMemorySize()))
        return false;
    return true;
}

ConvSolution
GroupNormForward::GetSolution(const ExecutionContext& context,
                              const miopen::groupnorm::ProblemDescription& problem) const
{
    std::ignore = context;

    auto result = ConvSolution{miopenStatusSuccess};

    {
        auto dtype = problem.GetXDesc().GetType();
        auto dims  = problem.GetXDesc().GetLengths();

        size_t numel             = problem.GetXDesc().GetElementSize();
        size_t num_groups        = problem.GetNumGroups();
        size_t outer_size        = dims[0] * num_groups;

        size_t xlocalsize = LOCAL_SIZE;
        size_t xgridsize  = outer_size * xlocalsize;
        size_t ylocalsize = 1;
        size_t ygridsize  = 1;
        size_t zlocalsize = 1;
        size_t zgridsize  = 1;

        auto kernel = KernelInfo{};

        kernel.kernel_file = "MIOpenGroupNorm.cpp";
        kernel.kernel_name = "GroupNormFwdContiguous";

        const auto build_params = KernelBuildParameters{
            {"MIOPEN_USE_FP16", static_cast<int>(dtype == miopenHalf)},
            {"MIOPEN_USE_FP32", static_cast<int>(dtype == miopenFloat)},
            {"MIOPEN_USE_FP64", static_cast<int>(dtype == miopenDouble)},
            {"MIOPEN_USE_BFP16", static_cast<int>(dtype == miopenBFloat16)},
            {"LOCAL_SIZE", LOCAL_SIZE},
        };

        kernel.comp_options = build_params.GenerateFor(kbp::HIP{});

        kernel.l_wk.push_back(xlocalsize);
        kernel.l_wk.push_back(ylocalsize);
        kernel.l_wk.push_back(zlocalsize);

        kernel.g_wk.push_back(xgridsize);
        kernel.g_wk.push_back(ygridsize);
        kernel.g_wk.push_back(zgridsize);

        result.construction_params.push_back(kernel);
    }

    result.invoker_factory = [](const std::vector<Kernel>& kernels) {
        return [=](const Handle& handle_, const AnyInvokeParams& raw_params) {
            decltype(auto) kernel = handle_.Run(kernels.front());
            decltype(auto) params = raw_params.CastTo<miopen::groupnorm::InvokeParams>();

            auto dims                = params.xDesc->GetLengths();
            size_t numel             = params.xDesc->GetElementSize();
            size_t numel_per_channel = numel / dims[0] / dims[1];
            size_t num_channels      = dims[1];

            kernel(params.x,
                   params.y,
                   params.weight,
                   params.bias,
                   params.mean,
                   params.rstd,
                   params.epsilon,
                   static_cast<size_t>(params.num_groups),
                   num_channels,
                   numel_per_channel,
                   static_cast<bool>(params.mode));
        };
    };

    return result;
}

} // namespace groupnorm

} // namespace solver

} // namespace miopen
