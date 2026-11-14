#pragma once

namespace Mandelbrot::detail
{
    const char* GetShallowShaderSource() noexcept;
    const char* GetPerturbLdsShaderSource() noexcept;
    const char* GetPerturbLdsBlaShaderSource() noexcept;
    const char* GetPerturbDeepExpShaderSource() noexcept;
    const char* GetMovieDetectShaderSource() noexcept;
    const char* GetMovieResolveShaderSource() noexcept;
}
