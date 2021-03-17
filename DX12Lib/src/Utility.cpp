#include <Utility.h>
#include <comdef.h>

DxException::DxException(HRESULT hr, const std::string& functionName, const std::string& filename, int lineNumber) :
    ErrorCode(hr), FunctionName(functionName), Filename(filename), LineNumber(lineNumber) {}

std::string DxException::ToString() const
{
    // Get the string description of the error code.
    _com_error err(ErrorCode);
    std::string msg = err.ErrorMessage();

    return FunctionName + " failed in " + Filename + "; line " + STRINGIFY_BUILTIN(LineNumber) + "; error: " + msg;
}
