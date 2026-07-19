#include <jni.h>
#include <string>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>
#include <dlfcn.h>

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
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
    JNIEnv *env, jobject, jstring jIRCode, jstring jIncludes, jstring jStdlib) {

    const char *ir = env->GetStringUTFChars(jIRCode, nullptr);
    env->ReleaseStringUTFChars(jIncludes, nullptr);
    env->ReleaseStringUTFChars(jStdlib, nullptr);

    std::string irStr(ir);
    env->ReleaseStringUTFChars(jIRCode, ir);

    return env->NewStringUTF(jitExecute(irStr).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mcompiladorcpp_MainActivity_compileOnlyNative(
    JNIEnv *env, jobject, jstring jSource, jstring jIncludes) {

    env->ReleaseStringUTFChars(jSource, nullptr);
    env->ReleaseStringUTFChars(jIncludes, nullptr);

    return env->NewStringUTF("ORCJIT listo. Use compileAndRunNative con LLVM IR.");
}
