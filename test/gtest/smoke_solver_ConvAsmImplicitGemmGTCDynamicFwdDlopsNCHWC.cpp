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
#include <tuple>
#include <string_view>

#include "gtest_common.hpp"

#include "../conv2d.hpp"

MIOPEN_DECLARE_ENV_VAR_BOOL(MIOPEN_TEST_GPU_XNACK_ENABLED)

namespace {

auto GetTestCases()
{
    const auto env_fwd =
        std::tuple{std::pair{ENV(MIOPEN_FIND_ENFORCE), std::string_view("SEARCH_DB_UPDATE")},
                   std::pair{ENV(MIOPEN_DEBUG_TUNING_ITERATIONS_MAX), std::string_view("5")},
                   std::pair{ENV(MIOPEN_FIND_MODE), std::string_view("normal")},
                   std::pair{ENV(MIOPEN_DEBUG_FIND_ONLY_SOLVER),
                             std::string_view("ConvAsmImplicitGemmGTCDynamicFwdDlopsNCHWC")}};

    const std::string vf = " --verbose --disable-backward-data --disable-backward-weights";
    const std::string layout =
        " --in_layout NCHW --fil_layout CHWN --out_layout NCHW --tensor_vect 1 --vector_length 4";

    return std::vector{
        // clang-format off
    std::pair{env_fwd, vf + " --input 64 256 7 7 --weights 256 3 3 128 --pads_strides_dilations 0 0 1 1 1 1" + layout}
        // clang-format on
    };
}

using TestCase = decltype(GetTestCases())::value_type;

bool SkipTest() { return miopen::IsEnabled(ENV(MIOPEN_TEST_GPU_XNACK_ENABLED)); }

bool IsTestSupportedForDevice()
{
    using e_mask = enabled<Gpu::gfx103X>;
    using d_mask = disabled<Gpu::gfx900, Gpu::gfx906, Gpu::gfx908, Gpu::gfx90A>;
    return ::IsTestSupportedForDevMask<d_mask, e_mask>();
}

} // namespace

class Conv2dTuningHalf : public HalfTestCase<std::vector<TestCase>>
{
};

TEST_P(Conv2dTuningHalf, HalfTest_smoke_solver_ConvAsmImplicitGemmGTCDynamicFwdDlopsNCHWC)
{
    if(IsTestSupportedForDevice() && !SkipTest())
    {
        invoke_with_params<conv2d_driver, Conv2dTuningHalf>(tuning_check);
    }
    else
    {
        GTEST_SKIP();
    }
};

INSTANTIATE_TEST_SUITE_P(SmokeSolverConvAsmImplicitGemmGTCDynamicFwdDlopsNhwc,
                         Conv2dTuningHalf,
                         testing::Values(GetTestCases()));
