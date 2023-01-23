/*
** Copyright (c) 2022 Advanced Micro Devices, Inc. All rights reserved.
**
** Permission is hereby granted, free of charge, to any person obtaining a
** copy of this software and associated documentation files (the "Software"),
** to deal in the Software without restriction, including without limitation
** the rights to use, copy, modify, merge, publish, distribute, sublicense,
** and/or sell copies of the Software, and to permit persons to whom the
** Software is furnished to do so, subject to the following conditions:
**
** The above copyright notice and this permission notice shall be included in
** all copies or substantial portions of the Software.
**
** THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
** IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
** FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
** AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
** LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
** FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
** DEALINGS IN THE SOFTWARE.
*/

#ifndef GFXRECON_DECODE_DRIVER_INFO_CONSUMER_H
#define GFXRECON_DECODE_DRIVER_INFO_CONSUMER_H

#include "decode/api_decoder.h"
#include "driver_info_consumer_base.h"
#include "util/platform.h"

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(decode)

static constexpr char kNoDriverInfoStr[] = "Driver info not available.\n";

class DriverInfoConsumer : public DriverInfoConsumerBase
{
  public:
    DriverInfoConsumer() {}

    const char* GetDriverDesc() const
    {
        if (strlen(driver_info_) == 0)
        {
            return kNoDriverInfoStr;
        }
        else
        {
            return driver_info_;
        }
    }

    virtual void Process_DriverInfo(const char* info_record)
    {
        util::platform::StringCopy(driver_info_,
                                   gfxrecon::util::filepath::kMaxDriverInfoSize,
                                   info_record,
                                   gfxrecon::util::filepath::kMaxDriverInfoSize);
        found_driver_info_ = true;
    }

    virtual bool IsComplete(uint64_t current_block_index) { return found_driver_info_; }

  private:
    char driver_info_[gfxrecon::util::filepath::kMaxDriverInfoSize] = {};
    bool found_driver_info_{ false };
};

GFXRECON_END_NAMESPACE(decode)
GFXRECON_END_NAMESPACE(gfxrecon)

#endif // GFXRECON_DECODE_DRIVER_INFO_CONSUMER_H
