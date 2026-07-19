#include <jni.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <dlfcn.h>

#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/Frontend/CompilerInvocation.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;
using namespace llvm::orc;

static jmp_buf jumpBuffer;
static volatile sig_atomic_t signalCaught = 0;

static void crashHandler(int sig) {
    signalCaught = sig;
    longjmp(jumpBuffer, 1);
}

static std::string compileToIR(const std::string &sourceCode,
                                const std::string &includePath,
                                std::string &errorMsg) {
    SmallString<256> tempSrc;
    if (auto ec = sys::fs::createTemporaryFile("code", "cpp", tempSrc)) {
        errorMsg = "Error creando archivo temporal";
        return "";
    }
    {
        raw_fd_ostream out(tempSrc, false);
        out << sourceCode;
    }

    SmallString<256> tempIR;
    if (auto ec = sys::fs::createTemporaryFile("output", "ll", tempIR)) {
        sys::fs::remove(tempSrc);
        errorMsg = "Error creando archivo temporal IR";
        return "";
    }

    auto diagOpts = clang::CreateDiagnosticOptions();
    IntrusiveRefCntPtr<clang::DiagnosticIDs> diagIDs(new clang::DiagnosticIDs());
    std::string diagString;
    llvm::raw_string_ostream diagStream(diagString);
    auto *diagPrinter = new clang::TextDiagnosticPrinter(diagStream, &*diagOpts);
    IntrusiveRefCntPtr<clang::DiagnosticsEngine> diags(
        new clang::DiagnosticsEngine(diagIDs, &*diagOpts, diagPrinter));

    std::vector<const char *> cArgs = {
        "-xc++",
        "-std=c++17",
        "-D__ANDROID__",
        "-D__BIONIC__",
        "-DANDROID",
        "-D__aarch64__",
        "-mno-outline-atomics",
        "-nostdinc",
        "-nostdlib",
        "-fno-exceptions",
        "-fno-rtti",
        "-fno-threadsafe-statics",
        "-w",
        "--target=aarch64-linux-android24",
        "-I", includePath.c_str(),
        "-emit-llvm",
        "-S",
        "-o", tempIR.c_str(),
        tempSrc.c_str()
    };

    auto invocation = std::make_unique<clang::CompilerInvocation>();
    if (!clang::CompilerInvocation::CreateFromArgs(*invocation, cArgs, *diags)) {
        sys::fs::remove(tempSrc);
        sys::fs::remove(tempIR);
        errorMsg = "Error creando CompilerInvocation: " + diagString;
        return "";
    }

    invocation->getLangOpts()->CPlusPlus = true;
    invocation->getLangOpts()->CPlusPlus17 = true;
    invocation->getLangOpts()->Exceptions = 0;
    invocation->getLangOpts()->RTTI = false;

    clang::CompilerInstance CI;
    CI.setDiagnostics(diags.get());
    CI.setInvocation(std::move(invocation));

    EmitLLVMOnlyAction action;
    if (!CI.ExecuteAction(action)) {
        sys::fs::remove(tempSrc);
        sys::fs::remove(tempIR);
        diagPrinter->Finish();
        errorMsg = "Error compilando:\n" + diagString;
        return "";
    }

    std::unique_ptr<Module> module = action.takeModule();
    if (!module) {
        sys::fs::remove(tempSrc);
        sys::fs::remove(tempIR);
        errorMsg = "Error generando modulo LLVM IR";
        return "";
    }

    module->setTargetTriple("aarch64-linux-android24");

    std::string irString;
    raw_string_ostream irStream(irString);
    irStream << *module;
    irStream.flush();

    sys::fs::remove(tempSrc);
    sys::fs::remove(tempIR);
    return irString;
}

static std::string jitExecute(const std::string &irCode) {
    InitializeNativeTarget();
    InitializeNativeAsmParser();

    auto context = std::make_unique<LLVMContext>();

    auto jit = LLJITBuilder().create();
    if (!jit) {
        return "Error creando JIT: " + toString(jit.takeError());
    }

    auto &JD = (*jit)->getMainJITDylib();
    auto Gen = DynamicLibrarySearchGenerator::GetForProcess(
        (*jit)->getDataLayout().getGlobalPrefix());
    if (!Gen) {
        return "Error creando symbol resolver: " + toString(Gen.takeError());
    }
    JD.add(std::move(*Gen));

    auto memBuf = MemoryBuffer::getMemBuffer(irCode);
    SMDiagnostic smDiag;
    auto module = parseIRFile(memBuf->getBufferIdentifier(), smDiag, *context);
    if (!module) {
        std::string errMsg;
        raw_string_ostream errStream(errMsg);
        smDiag.print("jit", errStream);
        return errMsg;
    }

    module->setTargetTriple("aarch64-linux-android24");

    auto tsm = ThreadSafeModule(std::move(module), std::move(context));
    if (auto err = (*jit)->addIRModule(std::move(tsm))) {
        return "Error agregando modulo: " + toString(std::move(err));
    }

    auto mainAddr = (*jit)->lookup("main");
    if (!mainAddr) {
        return "Error: 'main' no encontrada\n" + toString(mainAddr.takeError());
    }

    typedef int (*MainFunc)();
    MainFunc mainFunc = reinterpret_cast<MainFunc>(mainAddr->getAddress());

    struct sigaction sa_new, sa_old_segv, sa_old_fpe, sa_old_ill, sa_old_abrt;
    memset(&sa_new, 0, sizeof(sa_new));
    sa_new.sa_handler = crashHandler;
    sigemptyset(&sa_new.sa_mask);
    sigaction(SIGSEGV, &sa_new, &sa_old_segv);
    sigaction(SIGFPE, &sa_new, &sa_old_fpe);
    sigaction(SIGILL, &sa_new, &sa_old_ill);
    sigaction(SIGABRT, &sa_new, &sa_old_abrt);

    int old_stdout = dup(STDOUT_FILENO);
    int old_stderr = dup(STDERR_FILENO);
    int pipefd[2];
    pipe(pipefd);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    int exitCode = 0;
    std::string output;
    if (setjmp(jumpBuffer) == 0) {
        exitCode = mainFunc();
    } else {
        dup2(old_stdout, STDOUT_FILENO);
        dup2(old_stderr, STDERR_FILENO);
        close(old_stdout);
        close(old_stderr);
        close(pipefd[0]);
        sigaction(SIGSEGV, &sa_old_segv, nullptr);
        sigaction(SIGFPE, &sa_old_fpe, nullptr);
        sigaction(SIGILL, &sa_old_ill, nullptr);
        sigaction(SIGABRT, &sa_old_abrt, nullptr);
        return "Error de ejecucion: signal " + std::to_string(signalCaught);
    }

    fflush(stdout);
    fflush(stderr);
    dup2(old_stdout, STDOUT_FILENO);
    dup2(old_stderr, STDERR_FILENO);
    close(old_stdout);
    close(old_stderr);

    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf) - 1)) > 0) {
        buf[n] = '\0';
        output.append(buf, n);
    }
    close(pipefd[0]);

    sigaction(SIGSEGV, &sa_old_segv, nullptr);
    sigaction(SIGFPE, &sa_old_fpe, nullptr);
    sigaction(SIGILL, &sa_old_ill, nullptr);
    sigaction(SIGABRT, &sa_old_abrt, nullptr);

    output += "\n[Exit: " + std::to_string(exitCode) + "]";
    return output;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mcompiladorcpp_MainActivity_compileAndRunNative(
    JNIEnv *env, jobject, jstring jSource, jstring jIncludes, jstring jStdlib) {

    const char *source = env->GetStringUTFChars(jSource, nullptr);
    const char *includes = env->GetStringUTFChars(jIncludes, nullptr);
    env->ReleaseStringUTFChars(jStdlib, nullptr);

    std::string src(source);
    std::string inc(includes);
    env->ReleaseStringUTFChars(jSource, source);
    env->ReleaseStringUTFChars(jIncludes, includes);

    std::string errorMsg;
    std::string ir = compileToIR(src, inc, errorMsg);
    if (ir.empty()) {
        return env->NewStringUTF(errorMsg.c_str());
    }

    return env->NewStringUTF(jitExecute(ir).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mcompiladorcpp_MainActivity_compileOnlyNative(
    JNIEnv *env, jobject, jstring jSource, jstring jIncludes) {

    const char *source = env->GetStringUTFChars(jSource, nullptr);
    const char *includes = env->GetStringUTFChars(jIncludes, nullptr);

    std::string src(source);
    std::string inc(includes);
    env->ReleaseStringUTFChars(jSource, source);
    env->ReleaseStringUTFChars(jIncludes, includes);

    std::string errorMsg;
    std::string ir = compileToIR(src, inc, errorMsg);
    if (ir.empty()) {
        return env->NewStringUTF(errorMsg.c_str());
    }
    return env->NewStringUTF(ir.c_str());
}
