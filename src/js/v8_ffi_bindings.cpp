#include "mystral/js/v8_ffi_bindings.h"

#if defined(MYSTRAL_JS_V8)

#include "v8.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <infix/infix.h>
#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

namespace mystral {
namespace js {
namespace {

enum class FfiType {
    Void,
    Int,
    SizeT,
    Float,
    Double,
    String,
    Pointer,
    Buffer
};

struct FfiLibrary {
#ifdef _WIN32
    HMODULE module = nullptr;
#else
    void* module = nullptr;
#endif
    std::string path;
};

#ifdef _WIN32
using FfiAddress = FARPROC;
#else
using FfiAddress = void*;
#endif

struct FfiFunction {
    FfiLibrary* library = nullptr;
    FfiAddress address = nullptr;
    infix_forward_t* trampoline = nullptr;
    infix_cif_func call = nullptr;
    FfiType returnType = FfiType::Void;
    std::vector<FfiType> argumentTypes;
    std::string name;
};

struct ConvertedArgument {
    const char* stringValue = nullptr;
    std::string stringStorage;
    void* pointerValue = nullptr;
    int intValue = 0;
    size_t sizeValue = 0;
    float floatValue = 0.0f;
    double doubleValue = 0.0;
};

union ReturnValue {
    int intValue;
    size_t sizeValue;
    float floatValue;
    double doubleValue;
    void* pointerValue;
};

bool hasSuffix(const std::string& value, const char* suffix) {
    const std::string suffixString(suffix);
    return value.size() >= suffixString.size() &&
        value.compare(value.size() - suffixString.size(), suffixString.size(), suffixString) == 0;
}

std::string addUnixLibraryPrefix(const std::string& path) {
    const size_t separator = path.find_last_of("/\\");
    const size_t filenameStart = separator == std::string::npos ? 0 : separator + 1;
    if (path.compare(filenameStart, 3, "lib") == 0) {
        return path;
    }
    return path.substr(0, filenameStart) + "lib" + path.substr(filenameStart);
}

std::vector<std::string> libraryCandidates(const std::string& name) {
    if (hasSuffix(name, ".dll") || hasSuffix(name, ".so") || hasSuffix(name, ".dylib")) {
        return {name};
    }

#ifdef _WIN32
    return {name + ".dll"};
#elif defined(__APPLE__)
    return {addUnixLibraryPrefix(name) + ".dylib"};
#else
    return {addUnixLibraryPrefix(name) + ".so"};
#endif
}

FfiAddress findSymbol(FfiLibrary* library, const char* name) {
#ifdef _WIN32
    return GetProcAddress(library->module, name);
#else
    return dlsym(library->module, name);
#endif
}

bool loadLibrary(FfiLibrary* library) {
    for (const auto& candidate : libraryCandidates(library->path)) {
        library->path = candidate;
#ifdef _WIN32
        library->module = LoadLibraryA(library->path.c_str());
#else
        library->module = dlopen(library->path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (library->module) {
            return true;
        }
    }
    return false;
}

std::string libraryError() {
#ifdef _WIN32
    return "";
#else
    const char* error = dlerror();
    return error ? std::string(": ") + error : std::string();
#endif
}

v8::Local<v8::String> v8String(v8::Isolate* isolate, const char* value) {
    return v8::String::NewFromUtf8(isolate, value, v8::NewStringType::kNormal)
        .ToLocalChecked();
}

void throwError(v8::Isolate* isolate, const std::string& message) {
    isolate->ThrowException(v8::Exception::Error(v8String(isolate, message.c_str())));
}

bool parseType(v8::Isolate* isolate, v8::Local<v8::Value> value, FfiType& out) {
    v8::String::Utf8Value text(isolate, value);
    if (!*text) {
        throwError(isolate, "FFI type must be a string");
        return false;
    }

    const std::string name(*text);
    if (name == "void") out = FfiType::Void;
    else if (name == "int" || name == "int32") out = FfiType::Int;
    else if (name == "size_t" || name == "uint64") out = FfiType::SizeT;
    else if (name == "float") out = FfiType::Float;
    else if (name == "double") out = FfiType::Double;
    else if (name == "string") out = FfiType::String;
    else if (name == "pointer") out = FfiType::Pointer;
    else if (name == "buffer") out = FfiType::Buffer;
    else {
        throwError(isolate, "Unsupported FFI type: " + name);
        return false;
    }
    return true;
}

bool getPointer(v8::Isolate* isolate, v8::Local<v8::Value> value, void*& out) {
    if (value->IsExternal()) {
        out = value.As<v8::External>()->Value();
        return true;
    }

    if (value->IsNull() || value->IsUndefined()) {
        out = nullptr;
        return true;
    }

    throwError(isolate, "Expected an FFI pointer");
    return false;
}

bool convertArgument(v8::Isolate* isolate,
                     v8::Local<v8::Value> value,
                     FfiType type,
                     ConvertedArgument& out) {
    switch (type) {
    case FfiType::String: {
        if (!value->IsString()) {
            throwError(isolate, "Expected a string argument");
            return false;
        }
        v8::String::Utf8Value text(isolate, value);
        if (!*text) {
            throwError(isolate, "Could not convert string argument");
            return false;
        }
        out.stringStorage = *text;
        out.stringValue = out.stringStorage.c_str();
        return true;
    }
    case FfiType::Pointer:
        return getPointer(isolate, value, out.pointerValue);
    case FfiType::Buffer: {
        if (!value->IsArrayBufferView()) {
            throwError(isolate, "Expected an ArrayBuffer or TypedArray");
            return false;
        }
        auto view = value.As<v8::ArrayBufferView>();
        auto backing = view->Buffer()->GetBackingStore();
        out.pointerValue = static_cast<uint8_t*>(backing->Data()) + view->ByteOffset();
        return true;
    }
    case FfiType::Int:
        if (!value->IsNumber()) {
            throwError(isolate, "Expected an integer argument");
            return false;
        }
        out.intValue = value->Int32Value(isolate->GetCurrentContext()).FromMaybe(0);
        return true;
    case FfiType::SizeT:
        if (!value->IsNumber()) {
            throwError(isolate, "Expected a size_t argument");
            return false;
        }
        out.sizeValue = static_cast<size_t>(value->IntegerValue(isolate->GetCurrentContext()).FromMaybe(0));
        return true;
    case FfiType::Float:
        if (!value->IsNumber()) {
            throwError(isolate, "Expected a float argument");
            return false;
        }
        out.floatValue = static_cast<float>(value->NumberValue(isolate->GetCurrentContext()).FromMaybe(0));
        return true;
    case FfiType::Double:
        if (!value->IsNumber()) {
            throwError(isolate, "Expected a double argument");
            return false;
        }
        out.doubleValue = value->NumberValue(isolate->GetCurrentContext()).FromMaybe(0);
        return true;
    case FfiType::Void:
        throwError(isolate, "void is not valid as an argument type");
        return false;
    }
    return false;
}

const char* infixTypeName(FfiType type, bool returnType) {
    switch (type) {
    case FfiType::Void: return returnType ? "void" : nullptr;
    case FfiType::Int: return "int";
    case FfiType::SizeT: return "size_t";
    case FfiType::Float: return "float";
    case FfiType::Double: return "double";
    case FfiType::String: return returnType ? "*char" : "*char";
    case FfiType::Pointer:
    case FfiType::Buffer: return "*void";
    }
    return nullptr;
}

std::string makeSignature(FfiType returnType, const std::vector<FfiType>& argumentTypes) {
    std::ostringstream signature;
    signature << "(";
    for (size_t i = 0; i < argumentTypes.size(); ++i) {
        if (i != 0) signature << ", ";
        signature << infixTypeName(argumentTypes[i], false);
    }
    signature << ") -> " << infixTypeName(returnType, true);
    return signature.str();
}

void* argumentAddress(ConvertedArgument& argument, FfiType type) {
    switch (type) {
    case FfiType::String: return &argument.stringValue;
    case FfiType::Pointer:
    case FfiType::Buffer: return &argument.pointerValue;
    case FfiType::Int: return &argument.intValue;
    case FfiType::SizeT: return &argument.sizeValue;
    case FfiType::Float: return &argument.floatValue;
    case FfiType::Double: return &argument.doubleValue;
    case FfiType::Void: return nullptr;
    }
    return nullptr;
}

v8::Local<v8::Value> makeReturnValue(v8::Isolate* isolate, FfiType type, const ReturnValue& value) {
    switch (type) {
    case FfiType::Void: return v8::Undefined(isolate);
    case FfiType::Int: return v8::Integer::New(isolate, value.intValue);
    case FfiType::SizeT: return v8::BigInt::NewFromUnsigned(isolate, static_cast<uint64_t>(value.sizeValue));
    case FfiType::Float: return v8::Number::New(isolate, value.floatValue);
    case FfiType::Double: return v8::Number::New(isolate, value.doubleValue);
    case FfiType::Pointer: return v8::External::New(isolate, value.pointerValue);
    case FfiType::String: return v8String(isolate, value.pointerValue ? static_cast<const char*>(value.pointerValue) : "");
    case FfiType::Buffer: return v8::Undefined(isolate);
    }
    return v8::Undefined(isolate);
}

void invokeFunction(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* isolate = info.GetIsolate();
    v8::HandleScope handleScope(isolate);
    auto* function = static_cast<FfiFunction*>(info.Data().As<v8::External>()->Value());

    if (info.Length() != static_cast<int>(function->argumentTypes.size())) {
        throwError(isolate, function->name + " expects " +
            std::to_string(function->argumentTypes.size()) + " arguments");
        return;
    }

    std::vector<ConvertedArgument> args(function->argumentTypes.size());
    for (size_t i = 0; i < args.size(); ++i) {
        if (!convertArgument(isolate, info[static_cast<int>(i)], function->argumentTypes[i], args[i])) {
            return;
        }
    }

    std::vector<void*> argumentPointers(args.size());
    for (size_t i = 0; i < args.size(); ++i) {
        argumentPointers[i] = argumentAddress(args[i], function->argumentTypes[i]);
    }

    ReturnValue result{};
    function->call(&result, argumentPointers.empty() ? nullptr : argumentPointers.data());
    info.GetReturnValue().Set(makeReturnValue(isolate, function->returnType, result));
}

void functionCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* isolate = info.GetIsolate();
    v8::HandleScope handleScope(isolate);
    auto* library = static_cast<FfiLibrary*>(info.This()->GetInternalField(0).As<v8::External>()->Value());

    if (info.Length() != 3 || !info[0]->IsString() || !info[2]->IsArray()) {
        throwError(isolate, "ffi.function(name, returnType, argumentTypes) expected");
        return;
    }

    v8::String::Utf8Value name(isolate, info[0]);
    FfiAddress address = findSymbol(library, *name);
    if (!address) {
        throwError(isolate, "Symbol not found: " + std::string(*name ? *name : "") + libraryError());
        return;
    }

    auto* function = new FfiFunction();
    function->library = library;
    function->address = address;
    function->name = *name ? *name : "<ffi>";

    if (!parseType(isolate, info[1], function->returnType)) {
        delete function;
        return;
    }

    auto types = info[2].As<v8::Array>();
    for (uint32_t i = 0; i < types->Length(); ++i) {
        v8::Local<v8::Value> typeValue;
        if (!types->Get(isolate->GetCurrentContext(), i).ToLocal(&typeValue)) {
            delete function;
            return;
        }
        FfiType type;
        if (!parseType(isolate, typeValue, type)) {
            delete function;
            return;
        }
        function->argumentTypes.push_back(type);
    }

    const std::string signature = makeSignature(function->returnType, function->argumentTypes);
    const infix_status status = infix_forward_create(
        &function->trampoline, signature.c_str(), reinterpret_cast<void*>(address), nullptr);
    if (status != INFIX_SUCCESS) {
        const infix_error_details_t error = infix_get_last_error();
        const std::string message = error.message[0] ? error.message : "unknown infix error";
        const std::string functionName = function->name;
        delete function;
        throwError(isolate, "Could not create FFI trampoline for " + functionName + ": " + message);
        return;
    }
    function->call = infix_forward_get_code(function->trampoline);
    if (!function->call) {
        const std::string functionName = function->name;
        infix_forward_destroy(function->trampoline);
        delete function;
        throwError(isolate, "Could not get FFI trampoline for " + functionName);
        return;
    }

    auto functionTemplate = v8::FunctionTemplate::New(
        isolate, invokeFunction, v8::External::New(isolate, function));
    info.GetReturnValue().Set(functionTemplate->GetFunction(isolate->GetCurrentContext()).ToLocalChecked());
}

void openCallback(const v8::FunctionCallbackInfo<v8::Value>& info) {
    v8::Isolate* isolate = info.GetIsolate();
    v8::HandleScope handleScope(isolate);
    if (info.Length() != 1 || !info[0]->IsString()) {
        throwError(isolate, "ffi.open(libraryName) expected");
        return;
    }

    v8::String::Utf8Value path(isolate, info[0]);
    auto* library = new FfiLibrary();
    library->path = *path ? *path : "";
    if (!loadLibrary(library)) {
        delete library;
        throwError(isolate, "Could not load library: " + std::string(*path ? *path : "") + libraryError());
        return;
    }

    auto objectTemplate = v8::ObjectTemplate::New(isolate);
    objectTemplate->SetInternalFieldCount(1);
    auto object = objectTemplate->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
    object->SetInternalField(0, v8::External::New(isolate, library));
    object->Set(isolate->GetCurrentContext(), v8String(isolate, "function"),
        v8::FunctionTemplate::New(isolate, functionCallback)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).Check();
    info.GetReturnValue().Set(object);
}

}  // namespace

bool initV8FfiBindings(Engine* engine) {
    if (!engine || engine->getType() != EngineType::V8) {
        return false;
    }

    auto* isolate = static_cast<v8::Isolate*>(engine->getRawContext());
    auto* contextHandle = static_cast<v8::Global<v8::Context>*>(engine->getRawContextHandle());
    if (!isolate || !contextHandle) {
        return false;
    }
    v8::Isolate::Scope isolateScope(isolate);
    v8::HandleScope handleScope(isolate);
    auto context = contextHandle->Get(isolate);
    v8::Context::Scope contextScope(context);

    auto ffi = v8::Object::New(isolate);
    ffi->Set(context, v8String(isolate, "open"),
        v8::FunctionTemplate::New(isolate, openCallback)->GetFunction(context).ToLocalChecked()).Check();
    auto mystral = context->Global()->Get(context, v8String(isolate, "mystral"))
        .ToLocalChecked();
    v8::Local<v8::Object> mystralObject;
    if (mystral->IsObject()) {
        mystralObject = mystral.As<v8::Object>();
    } else {
        mystralObject = v8::Object::New(isolate);
        context->Global()->Set(context, v8String(isolate, "mystral"), mystralObject).Check();
    }
    mystralObject->Set(context, v8String(isolate, "ffi"), ffi).Check();
    return true;
}

}  // namespace js
}  // namespace mystral

#endif  // MYSTRAL_JS_V8
