/*
 *******************************************************************************
 *
 * Copyright (c) 2014-2017 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 ******************************************************************************/

/**
**************************************************************************************************
* @file  strings.h
**************************************************************************************************
*/

#ifndef __API_STRINGS_H__
#define __API_STRINGS_H__

#pragma once

#include <string.h>
#include <stdlib.h>
#include <stdint.h>

namespace vk
{

namespace secure
{
    namespace entry
    {
        enum EntryPointCondition : uint32_t
        {
            ENTRY_POINT_NONE,               // First-class entry point without any condition
            ENTRY_POINT_CORE,               // Core entry point specific to a core Vulkan version
            ENTRY_POINT_INSTANCE_EXTENSION, // Instance extension specific entry point
            ENTRY_POINT_DEVICE_EXTENSION,   // Device extension specific entry point
        };

        #include "open_strings/g_entry_points_decl.h"
    }

    namespace ext
    {
        #include "open_strings/g_extensions_decl.h"
    }
}

} // namespace vk

#endif /* __API_STRINGS_H__ */
