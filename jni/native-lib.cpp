#include <jni.h>
#include <unistd.h>
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Frontend/CompilerInvocation.h>
#include <clang/Frontend/TextDiagnosticPrinter.h>
#include <clang/CodeGen/CodeGenAction.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>

#include <string>
#include <cstring>
#include <setjmp.h>

using namespace llvm;
using namespace llvm::orc;

static jmp_buf jumpBuffer;
static volatile sig_atomic_t signalCaught = 0;

static void crashHandler(int sig) {
    signalCaught = sig;
    longjmp(jumpBuffer, 1);
}

struct PipeCapture {
    int old_stdout, old_stderr, pfd[2];
    void start() {
        fflush(stdout); fflush(stderr);
        old_stdout = dup(STDOUT_FILENO);
        old_stderr = dup(STDERR_FILENO);
        pipe(pfd);
        dup2(pfd[1], STDOUT_FILENO);
        dup2(pfd[1], STDERR_FILENO);
        close(pfd[1]);
    }
    std::string stop() {
        fflush(stdout); fflush(stderr);
        dup2(old_stdout, STDOUT_FILENO);
        dup2(old_stderr, STDERR_FILENO);
        close(old_stdout); close(old_stderr);
        std::string out;
        char buf[4096]; ssize_t n;
        while ((n = read(pfd[0], buf, sizeof(buf)-1)) > 0) { buf[n]='\0'; out.append(buf,n); }
        close(pfd[0]);
        return out;
    }
};

static std::string compileToIR(const std::string &source, const std::string &includes, std::string &err) {
    auto diagOpts = std::make_shared<clang::DiagnosticOptions>();
    IntrusiveRefCntPtr<clang::DiagnosticIDs> diagIDs(new clang::DiagnosticIDs());
    std::string diagStr;
    llvm::raw_string_ostream diagOS(diagStr);
    auto *printer = new clang::TextDiagnosticPrinter(diagOS, *diagOpts);
    IntrusiveRefCntPtr<clang::DiagnosticsEngine> diags(
        new clang::DiagnosticsEngine(diagIDs, *diagOpts, printer));

    std::vector<const char*> args = {
        "-xc++", "-std=c++17",
        "-D__ANDROID__", "-D__BIONIC__", "-DANDROID", "-D__aarch64__",
        "-mno-outline-atomics", "-nostdinc", "-nostdlib",
        "-fno-exceptions", "-fno-rtti", "-fno-threadsafe-statics", "-w",
        "--target=aarch64-linux-android24",
        "-I", includes.c_str()
    };

    auto inv = std::make_shared<clang::CompilerInvocation>();
    if (!clang::CompilerInvocation::CreateFromArgs(*inv, args, *diags)) {
        err = "CompilerInvocation failed: " + diagStr;
        return "";
    }
    inv->getLangOpts().CPlusPlus = true;
    inv->getLangOpts().CPlusPlus17 = true;
    inv->getLangOpts().Exceptions = 0;
    inv->getLangOpts().RTTI = false;

    clang::CompilerInstance CI(inv);
    CI.setDiagnostics(diags);

    clang::EmitLLVMOnlyAction action;
    if (!CI.ExecuteAction(action)) {
        err = "Clang error:\n" + diagStr;
        return "";
    }

    std::unique_ptr<Module> mod = action.takeModule();
    if (!mod) { err = "No LLVM module generated"; return ""; }

    mod->setTargetTriple(llvm::Triple("aarch64-linux-android24"));
    std::string ir;
    llvm::raw_string_ostream os(ir);
    os << *mod;
    return ir;
}

static std::string jitRun(const std::string &irCode) {
    InitializeNativeTarget();

    auto ctx = std::make_unique<LLVMContext>();
    auto jit = LLJITBuilder().create();
    if (!jit) return "JIT error: " + toString(jit.takeError());

    (*jit)->getPlatformJITDylib()->addGenerator(
        cantFail(orc::DynamicLibrarySearchGenerator::GetForCurrentProcess('\0')));

    auto buf = MemoryBuffer::getMemBuffer(irCode);
    SMDiagnostic smD;
    auto mod = parseIRFile(buf->getBufferIdentifier(), smD, *ctx);
    if (!mod) {
        std::string e;
        llvm::raw_string_ostream es(e);
        smD.print("jit", es);
        return e;
    }
    mod->setTargetTriple(llvm::Triple("aarch64-linux-android24"));

    if (auto err = (*jit)->addIRModule(ThreadSafeModule(std::move(mod), std::move(ctx))))
        return "Module error: " + toString(std::move(err));

    auto addr = (*jit)->lookup("main");
    if (!addr) return "main not found: " + toString(addr.takeError());

    typedef int (*Fn)();
    Fn mainFn = addr->toPtr<Fn>();

    struct sigaction sa, old_segv, old_fpe, old_ill, old_abrt;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = crashHandler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, &old_segv);
    sigaction(SIGFPE, &sa, &old_fpe);
    sigaction(SIGILL, &sa, &old_ill);
    sigaction(SIGABRT, &sa, &old_abrt);

    PipeCapture cap;
    cap.start();

    int exitCode = 0;
    std::string output;
    if (setjmp(jumpBuffer) == 0) {
        exitCode = mainFn();
    } else {
        output = cap.stop();
        sigaction(SIGSEGV, &old_segv, nullptr);
        sigaction(SIGFPE, &old_fpe, nullptr);
        sigaction(SIGILL, &old_ill, nullptr);
        sigaction(SIGABRT, &old_abrt, nullptr);
        return output + "\nCrash: signal " + std::to_string(signalCaught);
    }

    output = cap.stop();
    sigaction(SIGSEGV, &old_segv, nullptr);
    sigaction(SIGFPE, &old_fpe, nullptr);
    sigaction(SIGILL, &old_ill, nullptr);
    sigaction(SIGABRT, &old_abrt, nullptr);

    output += "\n[Exit: " + std::to_string(exitCode) + "]";
    return output;
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mcompiladorcpp_MainActivity_compileAndRunNative(
    JNIEnv *env, jobject, jstring jSrc, jstring jInc, jstring jStd) {
    const char *src = env->GetStringUTFChars(jSrc, nullptr);
    const char *inc = env->GetStringUTFChars(jInc, nullptr);
    env->ReleaseStringUTFChars(jStd, nullptr);
    std::string source(src), includes(inc);
    env->ReleaseStringUTFChars(jSrc, src);
    env->ReleaseStringUTFChars(jInc, inc);

    std::string err;
    std::string ir = compileToIR(source, includes, err);
    if (ir.empty()) return env->NewStringUTF(err.c_str());
    return env->NewStringUTF(jitRun(ir).c_str());
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_mcompiladorcpp_MainActivity_compileOnlyNative(
    JNIEnv *env, jobject, jstring jSrc, jstring jInc) {
    const char *src = env->GetStringUTFChars(jSrc, nullptr);
    const char *inc = env->GetStringUTFChars(jInc, nullptr);
    std::string source(src), includes(inc);
    env->ReleaseStringUTFChars(jSrc, src);
    env->ReleaseStringUTFChars(jInc, inc);

    std::string err;
    std::string ir = compileToIR(source, includes, err);
    if (ir.empty()) return env->NewStringUTF(err.c_str());
    return env->NewStringUTF(ir.c_str());
}
