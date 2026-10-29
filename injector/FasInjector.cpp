// 注入器
#define _GNU_SOURCE 1

#include <asm/ptrace.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifndef NT_PRSTATUS
#define NT_PRSTATUS 1
#endif

namespace {

struct Module {
    std::string path;
    std::uint64_t base = 0;
};

std::string readText(const std::string& path) {
    std::ifstream file(path);
    if (!file) return {};
    std::ostringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

bool isElfFile(const std::string& path) {
    if (path.empty() || path.front() != '/') return false;
    if (path.find("memfd:") != std::string::npos) return false;
    return true;
}

std::vector<Module> readModules(const pid_t pid) {
    std::vector<Module> result;
    std::istringstream maps(
        readText("/proc/" + std::to_string(pid) + "/maps"));
    std::string line;
    while (std::getline(maps, line)) {
        unsigned long long start = 0;
        unsigned long long end = 0;
        unsigned long long offset = 0;
        char permissions[8] = {};
        if (std::sscanf(line.c_str(), "%llx-%llx %7s %llx",
                        &start, &end, permissions, &offset) != 4)
            continue;
        const auto slash = line.find('/');
        if (slash == std::string::npos) continue;
        std::string path = line.substr(slash);
        if (!isElfFile(path)) continue;
        bool seen = false;
        for (const auto& module : result) {
            if (module.path == path) {
                seen = true;
                break;
            }
        }
        if (seen) continue;
        result.push_back({path, static_cast<std::uint64_t>(start) -
                                   static_cast<std::uint64_t>(offset)});
    }
    return result;
}

bool within(const std::string& data, const std::uint64_t offset,
            const std::uint64_t length) {
    return offset <= data.size() && length <= data.size() - offset;
}

std::optional<std::uint64_t> elfSymbolOffset(const std::string& path,
                                             const std::string& name) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return std::nullopt;
    const std::string data{std::istreambuf_iterator<char>(file),
                           std::istreambuf_iterator<char>()};
    if (data.size() < sizeof(Elf64_Ehdr)) return std::nullopt;

    Elf64_Ehdr header{};
    std::memcpy(&header, data.data(), sizeof(header));
    if (std::memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_machine != EM_AARCH64)
        return std::nullopt;

    auto section = [&](const std::uint64_t index, Elf64_Shdr& out) {
        if (index >= header.e_shnum) return false;
        const auto address = header.e_shoff + index * sizeof(Elf64_Shdr);
        if (!within(data, address, sizeof(Elf64_Shdr))) return false;
        std::memcpy(&out, data.data() + address, sizeof(out));
        return true;
    };

    for (std::uint64_t i = 0; i < header.e_shnum; ++i) {
        Elf64_Shdr symbols{};
        if (!section(i, symbols) || symbols.sh_type != SHT_DYNSYM) continue;
        if (symbols.sh_entsize != sizeof(Elf64_Sym) ||
            !within(data, symbols.sh_offset, symbols.sh_size))
            continue;
        Elf64_Shdr strings{};
        if (!section(symbols.sh_link, strings) ||
            !within(data, strings.sh_offset, strings.sh_size))
            continue;
        for (std::uint64_t j = 0; j < symbols.sh_size / sizeof(Elf64_Sym); ++j) {
            Elf64_Sym symbol{};
            std::memcpy(&symbol,
                        data.data() + symbols.sh_offset + j * sizeof(symbol),
                        sizeof(symbol));
            const auto type = ELF64_ST_TYPE(symbol.st_info);
            if (type != STT_FUNC && type != STT_GNU_IFUNC) continue;
            if (symbol.st_shndx == SHN_UNDEF || symbol.st_value == 0) continue;
            if (symbol.st_name >= strings.sh_size) continue;
            const char* text = data.data() + strings.sh_offset + symbol.st_name;
            if (std::strcmp(text, name.c_str()) != 0) continue;
            return symbol.st_value;
        }
    }
    return std::nullopt;
}

std::uint64_t remoteModuleBase(const pid_t pid, const std::string& name) {
    for (const auto& module : readModules(pid)) {
        if (module.path.find(name) != std::string::npos) return module.base;
    }
    return 0;
}

std::optional<std::uint64_t> remoteSymbol(const pid_t pid,
                                          const std::string& symbol) {
    const auto modules = readModules(pid);
    for (const std::string preferred : {"libc.so", "libdl.so"}) {
        for (const auto& module : modules) {
            if (module.path.find(preferred) == std::string::npos) continue;
            const auto offset = elfSymbolOffset(module.path, symbol);
            if (offset) return module.base + *offset;
        }
    }
    for (const auto& module : modules) {
        const auto offset = elfSymbolOffset(module.path, symbol);
        if (offset) return module.base + *offset;
    }
    return std::nullopt;
}

bool getRegs(const pid_t pid, user_pt_regs& regs) {
    iovec io{&regs, sizeof(regs)};
    return ptrace(PTRACE_GETREGSET, pid,
                  reinterpret_cast<void*>(NT_PRSTATUS), &io) == 0;
}

bool setRegs(const pid_t pid, const user_pt_regs& regs) {
    iovec io{const_cast<user_pt_regs*>(&regs), sizeof(regs)};
    return ptrace(PTRACE_SETREGSET, pid,
                  reinterpret_cast<void*>(NT_PRSTATUS), &io) == 0;
}

bool writeMemory(const pid_t pid, const std::uint64_t address,
                 const void* data, const std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t offset = 0; offset < size; offset += sizeof(long)) {
        long word = 0;
        const std::size_t remaining = size - offset;
        if (remaining >= sizeof(long)) {
            std::memcpy(&word, bytes + offset, sizeof(word));
        } else {
            errno = 0;
            word = ptrace(PTRACE_PEEKDATA, pid,
                          reinterpret_cast<void*>(address + offset), nullptr);
            if (word == -1 && errno != 0) return false;
            std::memcpy(&word, bytes + offset, remaining);
        }
        if (ptrace(PTRACE_POKEDATA, pid,
                   reinterpret_cast<void*>(address + offset),
                   reinterpret_cast<void*>(word)) < 0)
            return false;
    }
    return true;
}

// 等待停住
bool waitStopped(const pid_t pid, const int timeoutMs = 3000) {
    int status = 0;
    for (int waited = 0; waited < timeoutMs; ++waited) {
        const pid_t got = waitpid(pid, &status, WNOHANG);
        if (got == pid) return WIFSTOPPED(status);
        if (got < 0) return false;
        usleep(1000);
    }
    return false;
}

// 等待返回
bool waitCallStop(const pid_t pid, const std::uint64_t libcBase,
                  user_pt_regs& regs, const int timeoutMs = 3000) {
    int status = 0;
    bool stopped = false;
    for (int waited = 0; waited < timeoutMs; ++waited) {
        if (!stopped) {
            const pid_t got = waitpid(pid, &status, WNOHANG);
            if (got < 0) return false;
            if (got == 0) {
                usleep(1000);
                continue;
            }
            if (!WIFSTOPPED(status)) return false;
            stopped = true;
        }
        if (!getRegs(pid, regs)) return false;
        if (regs.pc == libcBase) return true;
        const int sig = WSTOPSIG(status);
        if (ptrace(PTRACE_CONT, pid, nullptr,
                   reinterpret_cast<void*>(
                       static_cast<long>(sig == SIGTRAP ? 0 : sig))) < 0)
            return false;
        stopped = false;
    }
    return false;
}

class Injector {
public:
    bool inject(const pid_t pid, const std::string& library,
                const std::string& function) {
        pid_ = pid;
        if (ptrace(PTRACE_SEIZE, pid_, nullptr, nullptr) < 0) {
            std::printf("[-] ptrace seize 失败: %s\n", std::strerror(errno));
            return false;
        }
        ptrace(PTRACE_INTERRUPT, pid_, nullptr, nullptr);
        if (!waitStopped(pid_) || !getRegs(pid_, original_)) {
            std::printf("[-] 读取寄存器失败\n");
            detachClean(false);
            return false;
        }

        libcBase_ = remoteModuleBase(pid_, "libc.so");
        if (!libcBase_) {
            std::printf("[-] 找不到远程 libc 基址\n");
            detachClean(true);
            return false;
        }

        bool ok = false;
        do {
            const auto mmapAddress = remoteSymbol(pid_, "mmap");
            const auto dlopenAddress = remoteSymbol(pid_, "dlopen");
            const auto dlsymAddress = remoteSymbol(pid_, "dlsym");
            std::printf("[+] libcBase=%p mmap=%p dlopen=%p dlsym=%p\n",
                        reinterpret_cast<void*>(libcBase_),
                        mmapAddress ? reinterpret_cast<void*>(*mmapAddress) : nullptr,
                        dlopenAddress ? reinterpret_cast<void*>(*dlopenAddress) : nullptr,
                        dlsymAddress ? reinterpret_cast<void*>(*dlsymAddress) : nullptr);
            if (!mmapAddress) break;
            std::uint64_t arguments[6] = {
                0, kScratchSize,
                static_cast<std::uint64_t>(PROT_READ | PROT_WRITE | PROT_EXEC),
                static_cast<std::uint64_t>(MAP_PRIVATE | MAP_ANONYMOUS),
                static_cast<std::uint64_t>(-1), 0};
            if (!callRemote(*mmapAddress, arguments, scratch_)) break;
            std::printf("[+] scratch=%p\n", reinterpret_cast<void*>(scratch_));
            if (!scratch_) {
                std::printf("[-] 远程 mmap 返回 0\n");
                break;
            }

            const auto pathAddress = scratch_ + 0x400;
            const auto nameAddress = scratch_ + 0x800;
            if (!writeString(pathAddress, library)) break;

            if (!dlopenAddress) break;
            std::uint64_t dlopenArguments[6] = {
                pathAddress,
                static_cast<std::uint64_t>(RTLD_NOW | RTLD_GLOBAL),
                0, 0, 0, 0};
            std::uint64_t handle = 0;
            if (!callRemote(*dlopenAddress, dlopenArguments, handle)) break;
            if (!handle) {
                std::printf("[-] 远程 dlopen 失败\n");
                break;
            }
            std::printf("[+] 远程 dlopen 成功: %p\n",
                        reinterpret_cast<void*>(handle));

            if (!function.empty()) {
                if (!writeString(nameAddress, function)) break;
                if (!dlsymAddress) break;
                std::uint64_t dlsymArguments[6] = {handle, nameAddress, 0, 0, 0, 0};
                std::uint64_t target = 0;
                if (!callRemote(*dlsymAddress, dlsymArguments, target)) break;
                if (!target) {
                    std::printf("[-] 远程 dlsym 失败: %s\n", function.c_str());
                    break;
                }
                std::uint64_t callArguments[6] = {0, 0, 0, 0, 0, 0};
                std::uint64_t result = 0;
                if (!callRemote(target, callArguments, result)) break;
                if (result != 0) {
                    std::printf("[-] %s 返回 %llu（目标符号未就绪）\n",
                                function.c_str(),
                                static_cast<unsigned long long>(result));
                    break;
                }
                std::printf("[+] %s 返回 0\n", function.c_str());
            }
            ok = true;
        } while (false);

        detachClean(true);
        return ok;
    }

private:
    static constexpr std::uint64_t kScratchSize = 0x20000;

    // 收尾清理
    void detachClean(const bool restore) {
        user_pt_regs probe{};
        if (!getRegs(pid_, probe)) {
            ptrace(PTRACE_INTERRUPT, pid_, nullptr, nullptr);
            if (!waitStopped(pid_, 500)) {
                kill(pid_, SIGSTOP);
                int status = 0;
                if (waitpid(pid_, &status, 0) != pid_ || !WIFSTOPPED(status)) return;
            }
        }
        if (restore) setRegs(pid_, original_);
        ptrace(PTRACE_DETACH, pid_, nullptr, nullptr);
        kill(pid_, SIGCONT);
    }

    // 远程调用
    bool callRemote(const std::uint64_t target,
                    const std::uint64_t (&args)[6],
                    std::uint64_t& result) {
        user_pt_regs regs{};
        if (!getRegs(pid_, regs)) return false;
        for (int i = 0; i < 6; ++i) regs.regs[i] = args[i];
        regs.pc = target;
        regs.regs[30] = libcBase_;
        if (!setRegs(pid_, regs)) return false;
        if (ptrace(PTRACE_CONT, pid_, nullptr, nullptr) < 0) return false;
        if (!waitCallStop(pid_, libcBase_, regs)) return false;
        result = regs.regs[0];
        return true;
    }

    bool writeString(const std::uint64_t address, const std::string& text) {
        return writeMemory(pid_, address, text.c_str(), text.size() + 1);
    }

    pid_t pid_ = -1;
    user_pt_regs original_{};
    std::uint64_t scratch_ = 0;
    std::uint64_t libcBase_ = 0;
};

}

int main(int argc, char** argv) {
    pid_t pid = 0;
    std::string library;
    std::string function;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-p" && i + 1 < argc) {
            pid = std::atoi(argv[++i]);
        } else if (argument == "-l" && i + 1 < argc) {
            library = argv[++i];
        } else if (argument == "-s" && i + 1 < argc) {
            function = argv[++i];
        } else {
            std::printf("用法: %s -p <pid> -l <库路径> [-s 函数名]\n", argv[0]);
            return 1;
        }
    }

    if (pid <= 0 || library.empty()) {
        std::printf("用法: %s -p <pid> -l <库路径> [-s 函数名]\n", argv[0]);
        return 1;
    }

    std::printf("[+] 注入 pid=%d 库=%s\n", pid, library.c_str());
    Injector injector;
    const bool ok = injector.inject(pid, library, function);
    std::printf(ok ? "[+] 注入完成\n" : "[-] 注入失败\n");
    return ok ? 0 : 1;
}
