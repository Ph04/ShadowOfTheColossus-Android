#include "on_device_pipeline.h"

#include "disc_inspector.h"
#include "elf_writer.h"
#include "iso9660.h"
#include "ps2recomp/ps2_recompiler.h"
#include "sha256.h"
#include "xff_linker.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {
using Clock = std::chrono::steady_clock;

constexpr uint32_t kUndefinedStub = 0x001B28F8u;
const char* kInputFiles[] = {
    "SCES_533.26",
    "KERNEL.XFF",
    "MANAGER.XFF",
    "GAMECORE.XFF",
    "STARTUP.XFF"
};
const uint32_t kExpectedSizes[] = {229888u, 419668u, 1732264u, 2157144u, 26u};
const char* kExpectedHashes[] = {
    "851f0c4f48b0b4bb61e89fb7e97d4e9b4ac4c2ea0ff143d1d95e18337ea8b57c",
    "d83a2d4d7631ed740946849b1b4ba32620891fc41817bb16175fedb9f9cd6463",
    "2de4d80eed7ff6c89b34b587ee1ced8792921e2a7c80d7e92a4fbb1481f1461a",
    "aa5a72b239b36da0bfc9f435bf31441057b4f4b8a01405b94b0e3b212fd242e0",
    "661bef7e10930b3470defd778e3cb811c61070bea51d82d341ad7dc5151701aa"
};

std::string jsonEscape(const std::string& text) {
    std::ostringstream out;
    for (unsigned char ch : text) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (ch < 0x20) out << '?';
            else out << static_cast<char>(ch);
            break;
        }
    }
    return out.str();
}

bool writeFile(const fs::path& path, const std::vector<uint8_t>& bytes, std::string& error) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Não foi possível criar pasta de preparação: " + ec.message();
        return false;
    }
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        error = "Não foi possível gravar " + path.filename().string() + ".";
        return false;
    }
    if (!bytes.empty()) {
        file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    if (!file) {
        error = "Falha ao gravar " + path.filename().string() + ".";
        return false;
    }
    return true;
}

size_t countGeneratedCpp(const fs::path& root) {
    std::error_code ec;
    if (!fs::exists(root, ec)) return 0;
    size_t count = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".cpp") ++count;
    }
    return count;
}

uint64_t generatedBytes(const fs::path& root) {
    std::error_code ec;
    uint64_t total = 0;
    if (!fs::exists(root, ec)) return 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    fs::recursive_directory_iterator end;
    for (; it != end && !ec; it.increment(ec)) {
        if (it->is_regular_file(ec) && it->path().extension() == ".cpp") {
            const auto size = it->file_size(ec);
            if (!ec) total += size;
        }
    }
    return total;
}

void publish(const SotcProgressCallback& callback, SotcPipelineProgress& state) {
    if (callback) callback(state);
}

void setStage(const SotcProgressCallback& callback, SotcPipelineProgress& state,
              int step, int percent, const std::string& stage, const std::string& detail) {
    state.step = step;
    state.percent = std::clamp(percent, 0, 99);
    state.stage = stage;
    state.detail = detail;
    state.etaSeconds = -1;
    publish(callback, state);
}

bool cancelled(const std::atomic_bool& flag, std::string& error) {
    if (!flag.load()) return false;
    error = "Preparação cancelada.";
    return true;
}

void savePipelineSummary(const fs::path& root, const ps2recomp::RecompilerReporter::Counters& counters,
                         size_t cppFiles, uint64_t cppBytes) {
    std::ostringstream json;
    json << "{\n"
         << "  \"profile\": \"SCES-53326 v1.00\",\n"
         << "  \"functions_discovered\": " << counters.functionsDiscovered << ",\n"
         << "  \"functions_processed\": " << counters.functionsProcessed << ",\n"
         << "  \"functions_recompiled\": " << counters.functionsRecompiled << ",\n"
         << "  \"functions_stubbed\": " << counters.functionsStubbed << ",\n"
         << "  \"functions_skipped\": " << counters.functionsSkipped << ",\n"
         << "  \"decode_failures\": " << counters.decodeFailures << ",\n"
         << "  \"unhandled_instructions\": " << counters.unhandledInstructions << ",\n"
         << "  \"generated_cpp_files\": " << cppFiles << ",\n"
         << "  \"generated_cpp_bytes\": " << cppBytes << ",\n"
         << "  \"note\": \"Código C++ gerado no dispositivo; ainda requer integração com o runtime PS2 e backend GS Android.\"\n"
         << "}\n";
    std::ofstream file(root / "pipeline_result.json", std::ios::binary | std::ios::trunc);
    if (file) file << json.str();
}

bool runRecompiler(const fs::path& outputRoot, const SotcProgressCallback& callback,
                   SotcPipelineProgress& progress, const std::atomic_bool& cancelFlag,
                   std::string& error) {
#if !defined(SOTC_HAS_PS2_RECOMPILER)
    (void)outputRoot; (void)callback; (void)progress; (void)cancelFlag;
    error = "O PS2Recomp não foi incluído nesta compilação do aplicativo.";
    return false;
#else
    const fs::path configPath = outputRoot / "sotc_recomp.toml";
    const fs::path generatedPath = outputRoot / "generated";
    setStage(callback, progress, 5, 54, "Inicializando PS2Recomp",
             "Lendo ELF, seções e símbolos no ARM64 do telefone...");
    ps2recomp::PS2Recompiler recompiler(configPath.string());
    if (!recompiler.initialize()) {
        recompiler.printReport();
        error = "O PS2Recomp não conseguiu inicializar a análise do ELF. Confira o relatório exportado.";
        return false;
    }
    auto initial = recompiler.reportCountersSnapshot();
    const size_t totalFunctions = std::max<size_t>(1, initial.functionsDiscovered);
    progress.percent = 60;
    progress.step = 5;
    progress.stage = "ELF analisado";
    progress.detail = std::to_string(initial.functionsDiscovered) +
                     " funções descobertas; preparando a recompilação ARM64.";
    publish(callback, progress);

    if (cancelled(cancelFlag, error)) return false;
    setStage(callback, progress, 6, 61, "Recompilando funções",
             "Processando funções MIPS no processador ARM64...");
    const auto recompileStart = Clock::now();
    std::atomic_bool monitorDone{false};
    std::thread monitor([&]() {
        while (!monitorDone.load()) {
            const auto counters = recompiler.reportCountersSnapshot();
            const size_t processed = std::min(counters.functionsProcessed, totalFunctions);
            const int localPercent = 61 + static_cast<int>((processed * 27u) / totalFunctions);
            progress.percent = std::clamp(localPercent, 61, 88);
            progress.step = 6;
            progress.stage = "Recompilando funções";
            progress.detail = std::to_string(processed) + " de " +
                std::to_string(totalFunctions) + " funções processadas (" +
                std::to_string(counters.functionsRecompiled) + " recompiladas).";
            if (processed > 0 && processed < totalFunctions) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                    Clock::now() - recompileStart).count();
                progress.etaSeconds = static_cast<int64_t>(
                    (static_cast<uint64_t>(std::max<int64_t>(0, elapsed)) *
                     (totalFunctions - processed)) / processed);
            } else {
                progress.etaSeconds = -1;
            }
            publish(callback, progress);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
    });

    const bool recompiled = recompiler.recompile();
    monitorDone.store(true);
    monitor.join();
    if (!recompiled) {
        recompiler.printReport();
        error = "A recompilação de código C++ falhou. O relatório completo está disponível para exportação.";
        return false;
    }
    const auto afterRecompile = recompiler.reportCountersSnapshot();
    progress.percent = 89;
    progress.step = 7;
    progress.stage = "Preparando arquivos C++";
    progress.detail = std::to_string(afterRecompile.functionsRecompiled) +
                      " funções recompiladas; gravando arquivos no armazenamento do aplicativo.";
    publish(callback, progress);

    if (cancelled(cancelFlag, error)) return false;
    const size_t expectedFiles = std::max<size_t>(1,
        afterRecompile.functionsRecompiled + afterRecompile.functionsStubbed);
    std::atomic_bool outputMonitorDone{false};
    std::thread outputMonitor([&]() {
        while (!outputMonitorDone.load()) {
            const size_t files = countGeneratedCpp(generatedPath);
            const int localPercent = 89 + static_cast<int>(
                (std::min(files, expectedFiles) * 10u) / expectedFiles);
            progress.percent = std::clamp(localPercent, 89, 98);
            progress.step = 7;
            progress.stage = "Gravando código C++";
            progress.detail = std::to_string(files) + " arquivos C++ gerados.";
            if (files > 0 && files < expectedFiles) {
                progress.etaSeconds = -1;
            }
            publish(callback, progress);
            std::this_thread::sleep_for(std::chrono::milliseconds(700));
        }
    });
    recompiler.generateOutput();
    outputMonitorDone.store(true);
    outputMonitor.join();

    const size_t cppFiles = countGeneratedCpp(generatedPath);
    const uint64_t cppBytes = generatedBytes(generatedPath);
    const auto finalCounters = recompiler.reportCountersSnapshot();
    savePipelineSummary(outputRoot, finalCounters, cppFiles, cppBytes);
    if (cppFiles == 0) {
        error = "O PS2Recomp terminou sem gerar arquivos C++. O relatório foi preservado para diagnóstico.";
        return false;
    }
    progress.percent = 99;
    progress.step = 7;
    progress.stage = "Código C++ gerado";
    progress.detail = std::to_string(cppFiles) + " arquivos C++ (" +
        std::to_string(cppBytes / (1024u * 1024u)) + " MiB).";
    progress.etaSeconds = 0;
    publish(callback, progress);
    error.clear();
    return true;
#endif
}
}

bool runSotcOnDevicePipeline(const IsoSource& source,
                             const std::string& appFilesDirectory,
                             std::atomic_bool& cancelRequested,
                             const SotcProgressCallback& onProgress,
                             std::string& error) {
    SotcPipelineProgress progress;
    progress.running = true;
    progress.outputDirectory = (fs::path(appFilesDirectory) / "sotc_pipeline" / "SCES-53326-v1.00").string();
    const auto start = Clock::now();
    const fs::path outputRoot(progress.outputDirectory);
    const fs::path inputDirectory = outputRoot / "input";
    error.clear();

    setStage(onProgress, progress, 1, 2, "Verificando a imagem",
             "Conferindo região, versão, executável e hashes dos módulos...");
    if (cancelled(cancelRequested, error)) return false;

    DiscInspection inspection;
    if (!inspectShadowOfTheColossusDisc(source, inspection, error)) return false;
    if (!inspection.supported) {
        error = "A imagem selecionada não é a edição PAL compatível.";
        return false;
    }
    progress.percent = 9;
    progress.detail = inspection.summary;
    publish(onProgress, progress);

    std::error_code ec;
    fs::remove_all(outputRoot, ec);
    ec.clear();
    fs::create_directories(inputDirectory, ec);
    if (ec) {
        error = "Não foi possível criar o armazenamento interno da preparação: " + ec.message();
        return false;
    }

    setStage(onProgress, progress, 2, 10, "Extraindo arquivos",
             "Copiando o ELF de boot e os três módulos XFF para o armazenamento privado do aplicativo...");
    Iso9660Reader iso(source);
    if (!iso.open(error)) return false;
    const uint64_t totalBytes = static_cast<uint64_t>(kExpectedSizes[0]) + kExpectedSizes[1] +
                                kExpectedSizes[2] + kExpectedSizes[3] + kExpectedSizes[4];
    uint64_t copiedBytes = 0;
    std::vector<std::vector<uint8_t>> inputs;
    inputs.reserve(5);
    for (size_t i = 0; i < 5; ++i) {
        if (cancelled(cancelRequested, error)) return false;
        Iso9660Entry entry;
        if (!iso.find(kInputFiles[i], entry) || entry.directory || entry.size != kExpectedSizes[i]) {
            error = std::string("Arquivo obrigatório ausente ou com tamanho inesperado: ") + kInputFiles[i];
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!iso.readFile(entry, bytes, kExpectedSizes[i], error)) return false;
        const std::string digest = Sha256::hex(bytes.data(), bytes.size());
        if (digest != kExpectedHashes[i]) {
            error = std::string(kInputFiles[i]) + ": SHA-256 divergiu durante a extração.";
            return false;
        }
        if (!writeFile(inputDirectory / kInputFiles[i], bytes, error)) return false;
        copiedBytes += bytes.size();
        inputs.push_back(std::move(bytes));
        progress.percent = 10 + static_cast<int>((copiedBytes * 10u) / totalBytes);
        progress.stage = "Extraindo arquivos";
        progress.detail = std::string("Extraído ") + kInputFiles[i] + " (" +
            std::to_string(copiedBytes / 1024u) + " de " + std::to_string(totalBytes / 1024u) + " KiB).";
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - start).count();
        if (copiedBytes > 0 && progress.percent > 10) {
            progress.etaSeconds = static_cast<int64_t>(
                (static_cast<uint64_t>(std::max<int64_t>(0, elapsed)) * (totalBytes - copiedBytes)) / copiedBytes);
        }
        publish(onProgress, progress);
    }

    if (cancelled(cancelRequested, error)) return false;
    setStage(onProgress, progress, 3, 22, "Lendo módulos XFF2",
             "Validando seções, símbolos, strings e tabelas de relocação...");
    const char* moduleNames[] = {"KERNEL", "MANAGER", "GAMECORE"};
    std::vector<XffModule> modules;
    modules.reserve(3);
    for (size_t i = 0; i < 3; ++i) {
        XffModule module;
        if (!parseXffModule(moduleNames[i], inputs[i + 1], module, error)) return false;
        modules.push_back(std::move(module));
        progress.percent = 22 + static_cast<int>((i + 1) * 6);
        progress.detail = std::string("Módulo ") + moduleNames[i] + " validado (" +
                          std::to_string(i + 1) + "/3).";
        publish(onProgress, progress);
    }

    if (cancelled(cancelRequested, error)) return false;
    setStage(onProgress, progress, 3, 42, "Ligando módulos",
             "Aplicando relocações MIPS e resolvendo símbolos entre KERNEL, MANAGER e GAMECORE...");
    const std::vector<uint32_t> loadBases = {0x001A5400u, 0x0116BC40u, 0x013442C0u};
    const std::vector<uint32_t> bssBases = {0x001EFA00u, 0x0131E040u, 0x012E5540u};
    XffLinkResult linked;
    if (!linkXffModules(modules, loadBases, bssBases, kUndefinedStub, linked, error)) return false;
    progress.percent = 49;
    progress.detail = std::to_string(linked.exports.size()) + " exports resolvidos; " +
                      std::to_string(linked.unresolvedImports.size()) + " imports permanecem dinâmicos.";
    publish(onProgress, progress);

    if (cancelled(cancelRequested, error)) return false;
    setStage(onProgress, progress, 4, 50, "Criando ELF unificado",
             "Juntando o executável de boot com as seções XFF relocadas e a tabela de símbolos...");
    SotcElfResult image;
    if (!buildSotcElf(inputs[0], modules, linked, image, error)) return false;
    if (!writeSotcRecompilerInputs(image, modules, linked, outputRoot.string(), error)) return false;
    progress.percent = 53;
    progress.detail = "ELF32/MIPS, mapa de símbolos, lista de imports e configuração do recompilador prontos.";
    publish(onProgress, progress);

    if (cancelled(cancelRequested, error)) return false;
    if (!runRecompiler(outputRoot, onProgress, progress, cancelRequested, error)) return false;

    progress.running = false;
    progress.completed = true;
    progress.failed = false;
    progress.percent = 100;
    progress.stage = "Preparação concluída";
    progress.detail = "ELF e código C++ gerados. Resultados disponíveis para exportação.";
    progress.etaSeconds = 0;
    publish(onProgress, progress);
    error.clear();
    return true;
}
