/*
** Copyright (c) 2020 LunarG, Inc.
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

#ifndef GFXRECON_APPLICATION_XLIB_APPLICATION_H
#define GFXRECON_APPLICATION_XLIB_APPLICATION_H

#include "application/application.h"
#include "util/defines.h"
#include "util/xlib_loader.h"

GFXRECON_BEGIN_NAMESPACE(gfxrecon)
GFXRECON_BEGIN_NAMESPACE(application)

class XlibWindow;

class XlibApplication : public Application
{
  public:
    XlibApplication(const std::string& name);

    virtual ~XlibApplication() override;

    const util::XlibLoader::FunctionTable& GetXlibFunctionTable() const { return xlib_loader_.GetFunctionTable(); }

    Display* OpenDisplay();

    void CloseDisplay(Display* display);

    virtual bool Initialize(decode::FileProcessor* file_processor) override;

    bool RegisterXlibWindow(XlibWindow* window);

    bool UnregisterXlibWindow(XlibWindow* window);

    virtual void ProcessEvents(bool wait_for_input) override;

  private:
    Display*         display_;
    size_t           display_open_count_;
    util::XlibLoader xlib_loader_;
};

GFXRECON_END_NAMESPACE(application)
GFXRECON_END_NAMESPACE(gfxrecon)

#endif // GFXRECON_APPLICATION_XLIB_APPLICATION_H