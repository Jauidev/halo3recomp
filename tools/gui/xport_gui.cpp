// Xcompiler's window: pick a game, run tools/port.py on it, watch its progress and read its report.
// Everything the tools print also goes to <work dir>/gui.log, and every step keeps its own log in
// <work dir>/logs, so a run can be read without the window. Dear ImGui on Win32 + DirectX 11, in
// the logo's colours.
#define IMGUI_DEFINE_MATH_OPERATORS
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace fs = std::filesystem;

static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_target = nullptr;
static UINT g_resizeWidth = 0, g_resizeHeight = 0;
static float g_scale = 1.0f;
static std::string g_dropped;  // a file or folder dropped on the window, for the next frame

static struct
{
    ImFont* body;
    ImFont* bold;
    ImFont* note;  // small text: field names, hints, the steps' names
    ImFont* heading;
    ImFont* title;
    ImFont* mono;
} g_fonts;

static ImVec4 Hex(unsigned rgb, float alpha = 1.0f)
{
    return ImVec4(float((rgb >> 16) & 255) / 255.0f, float((rgb >> 8) & 255) / 255.0f, float(rgb & 255) / 255.0f, alpha);
}

// The logo's colours: near-black navy, the X's cyan-to-blue and white.
namespace colour
{
const ImVec4 back = Hex(0x090D18), panel = Hex(0x0C1220), card = Hex(0x111A2C), well = Hex(0x0B111F), line = Hex(0x1D2840),
             field = Hex(0x080D1A), raised = Hex(0x18233A), text = Hex(0xE8EDF7), soft = Hex(0xC3CCDD), dim = Hex(0x8C98B3),
             faint = Hex(0x56627E), white = Hex(0xFFFFFF), cyan = Hex(0x29C6FF), blue = Hex(0x2F7CFF), deep = Hex(0x1F5BF5),
             ok = Hex(0x34D399), bad = Hex(0xF87171), warn = Hex(0xFBBF24);
}

// A colour for the draw list, faded like the widgets when they are disabled.
static ImU32 U32(const ImVec4& c, float alpha = 1.0f)
{
    return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha));
}

static ImVec4 Mix(const ImVec4& a, const ImVec4& b, float t)
{
    return ImVec4(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
}

static std::wstring Wide(const std::string& text)
{
    std::wstring out(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()));
    return out;
}

static std::string Utf8(const std::wstring& text)
{
    std::string out(WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), int(out.size()), nullptr, nullptr);
    return out;
}

// The Xcompiler folder: the first parent of the exe that holds tools/port.py.
static fs::path FindRoot()
{
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    for (fs::path dir = fs::path(exe).parent_path(); dir.has_parent_path() && dir != dir.parent_path(); dir = dir.parent_path())
    {
        if (fs::exists(dir / "tools" / "port.py"))
            return dir;
    }
    return fs::current_path();
}

// A running tool: its output, read on a thread, handed to the window and appended to the log file.
struct Run
{
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    std::thread reader;
    std::mutex mutex;
    std::string text;  // read but not yet taken by the window
    std::atomic<bool> running{false};
    std::atomic<bool> finished{false};
    DWORD exitCode = 0;

    bool Start(const std::wstring& commandLine, const fs::path& cwd, const fs::path& logPath)
    {
        Wait();
        {
            std::lock_guard lock(mutex);
            text.clear();
        }
        SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
        HANDLE readEnd = nullptr, writeEnd = nullptr;
        if (!CreatePipe(&readEnd, &writeEnd, &inherit, 0))
            return false;
        SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
        HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);

        STARTUPINFOW startup = {sizeof(startup)};
        startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
        startup.hStdInput = nul;
        startup.hStdOutput = writeEnd;
        startup.hStdError = writeEnd;
        PROCESS_INFORMATION info = {};
        std::wstring command = commandLine;
        const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, cwd.c_str(), &startup, &info);
        CloseHandle(writeEnd);
        CloseHandle(nul);
        if (!created)
        {
            CloseHandle(readEnd);
            std::lock_guard lock(mutex);
            text = "No se pudo lanzar: " + Utf8(commandLine) + "\n";
            return false;
        }
        // The job takes the whole tree (Python, the build, the game) down with Stop().
        job = CreateJobObjectW(nullptr, nullptr);
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = {};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
        AssignProcessToJobObject(job, info.hProcess);
        ResumeThread(info.hThread);
        CloseHandle(info.hThread);
        process = info.hProcess;
        running = true;
        finished = false;

        std::error_code error;
        fs::create_directories(logPath.parent_path(), error);
        reader = std::thread([this, readEnd, logPath] {
            std::ofstream log(logPath, std::ios::binary | std::ios::app);
            char buffer[4096];
            DWORD got = 0;
            while (ReadFile(readEnd, buffer, sizeof(buffer), &got, nullptr) && got)
            {
                log.write(buffer, got);
                log.flush();
                std::lock_guard lock(mutex);
                text.append(buffer, got);
            }
            CloseHandle(readEnd);
            WaitForSingleObject(process, INFINITE);
            GetExitCodeProcess(process, &exitCode);
            running = false;
            finished = true;
        });
        return true;
    }

    void Stop()
    {
        if (running && job)
            TerminateJobObject(job, 1);
    }

    void Wait()
    {
        if (reader.joinable())
            reader.join();
        if (process)
            CloseHandle(process);
        if (job)
            CloseHandle(job);
        process = job = nullptr;
    }

    // Moves what the tool printed since the last call to the end of out.
    void Take(std::string& out)
    {
        std::lock_guard lock(mutex);
        out += text;
        text.clear();
    }
};

// What port.py itself printed on a line ("[12:34:56] 3/7 Shaders del disco"), or nothing.
static std::string_view PortSaid(std::string_view line)
{
    if (line.size() < 12 || line[0] != '[' || line[3] != ':' || line[9] != ']' || line[10] != ' ')
        return {};
    return line.substr(11);
}

// The step a line of port.py starts: 1-7 (5b is 5), 8 for the phone, 0 if none.
static int StepOf(std::string_view said)
{
    if (said.size() > 4 && said[0] >= '1' && said[0] <= '7' && (said.substr(1, 3) == "/7 " || said.substr(1, 4) == "b/7 "))
        return said[0] - '0';
    return said.starts_with("Probando el APK en el móvil") ? 8 : 0;
}

// The run's output as lines, and what port.py's own lines say about the run.
struct Log
{
    std::string text;
    std::vector<size_t> lines{0};  // where each line starts
    int step = 0;                  // the step under way, as StepOf
    bool failed = false;           // a step ended in "falla"
    std::string last;              // port.py's last line

    bool Update(Run& run)
    {
        const size_t from = text.size();
        run.Take(text);
        for (size_t i = from; i < text.size(); ++i)
        {
            if (text[i] == '\n')
            {
                Read(std::string_view(text).substr(lines.back(), i - lines.back()));
                lines.push_back(i + 1);
            }
        }
        return text.size() != from;
    }

    std::string_view Line(size_t i) const
    {
        const size_t end = i + 1 < lines.size() ? lines[i + 1] : text.size();
        std::string_view line = std::string_view(text).substr(lines[i], end - lines[i]);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.remove_suffix(1);
        return line;
    }

    void Read(std::string_view line)
    {
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        const std::string_view said = PortSaid(line);
        if (said.empty())
            return;
        last = said;
        if (const int n = StepOf(said))
            step = n;
        else if (said.find(": falla") != std::string_view::npos)
            failed = true;
    }
};

static bool Has(std::string_view line, std::string_view word)
{
    return std::search(line.begin(), line.end(), word.begin(), word.end(),
                       [](char a, char b) { return std::tolower((unsigned char)a) == b; }) != line.end();
}

// A log line's colour: port.py's steps stand out, errors are red and warnings amber.
static ImVec4 LineColour(std::string_view line)
{
    if (const std::string_view said = PortSaid(line); !said.empty())
    {
        if (said.find(": falla") != std::string_view::npos)
            return colour::bad;
        return StepOf(said) || said.starts_with("Informe:") ? colour::cyan : colour::ok;
    }
    if (Has(line, "error") || Has(line, "traceback") || Has(line, "falla") || Has(line, "fatal"))
        return colour::bad;
    if (Has(line, "warning") || Has(line, "aviso"))
        return colour::warn;
    return colour::soft;
}

// report.md as the window draws it. Inline marks (** and `) are dropped.
struct Block
{
    char kind;         // '1'-'3' heading, '-' list item, 'n' numbered item, 'p' paragraph, 'c' code, 't' table
    int indent = 0;    // a list item's depth
    std::string mark;  // a numbered item's number
    std::string text;
};

static std::string Plain(std::string_view line)
{
    std::string out;
    for (size_t i = 0; i < line.size(); ++i)
    {
        if (line[i] == '`')
            continue;
        if (line[i] == '*' && i + 1 < line.size() && line[i + 1] == '*')
        {
            ++i;
            continue;
        }
        out += line[i];
    }
    return out;
}

static std::vector<Block> ParseReport(const std::string& text)
{
    std::vector<Block> blocks;
    std::istringstream in(text);
    std::string raw;
    bool fence = false, first = false;
    char previous = 0;  // the kind of the line before, for paragraphs and tables that go on
    while (std::getline(in, raw))
    {
        std::string_view line(raw);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (line.starts_with("```"))
        {
            fence = !fence;
            if (fence)
                blocks.push_back({'c'});
            first = true;
            previous = 0;
            continue;
        }
        if (fence)
        {
            if (!first)
                blocks.back().text += '\n';
            blocks.back().text += line;
            first = false;
            continue;
        }
        size_t spaces = 0, hashes = 0, digits = 0;
        while (spaces < line.size() && line[spaces] == ' ')
            ++spaces;
        const std::string_view body = line.substr(spaces);
        while (hashes < body.size() && body[hashes] == '#')
            ++hashes;
        while (digits < body.size() && std::isdigit((unsigned char)body[digits]))
            ++digits;
        char kind = 0;
        if (body.empty())
            kind = 0;
        else if (!spaces && hashes >= 1 && hashes <= 3 && body.size() > hashes && body[hashes] == ' ')
            blocks.push_back({kind = char('0' + hashes), 0, "", Plain(body.substr(hashes + 1))});
        else if (body.starts_with("- ") || body.starts_with("* "))
            blocks.push_back({kind = '-', int(spaces / 2), "", Plain(body.substr(2))});
        else if (digits && body.size() > digits + 1 && body[digits] == '.' && body[digits + 1] == ' ')
            blocks.push_back({kind = 'n', int(spaces / 2), std::string(body.substr(0, digits + 1)), Plain(body.substr(digits + 2))});
        else if (body.starts_with("|") || spaces >= 4)
        {
            kind = 't';
            if (previous == 't')
                blocks.back().text += "\n" + std::string(line);
            else
                blocks.push_back({'t', 0, "", std::string(line)});
        }
        else if (previous == 'p')
            blocks.back().text += " " + Plain(body), kind = 'p';
        else
            blocks.push_back({kind = 'p', 0, "", Plain(body)});
        previous = kind;
    }
    return blocks;
}

static void Wrapped(const std::string& text, const ImVec4& colour)
{
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str(), text.c_str() + text.size());
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// Monospaced text on a darker box, as wide as it needs.
static void Code(const std::string& text)
{
    const float s = g_scale, pad = 10 * s;
    ImFont* font = g_fonts.mono;
    const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text.c_str(), text.c_str() + text.size());
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const ImVec2 box(std::max(ImGui::GetContentRegionAvail().x, size.x + 2 * pad), size.y + 2 * pad);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, at + box, U32(colour::field), 6 * s);
    draw->AddText(font, font->FontSize, at + ImVec2(pad, pad), U32(colour::soft), text.c_str(), text.c_str() + text.size());
    ImGui::Dummy(box);
}

// The report: headings with a bar, lists with dots, and "Qué hacer ahora" on an amber card of its own.
static void DrawReport(const std::vector<Block>& blocks)
{
    const float s = g_scale;
    bool todo = false;
    for (const Block& block : blocks)
    {
        if (todo && (block.kind == '1' || block.kind == '2'))
        {
            ImGui::EndChild();
            todo = false;
        }
        const ImVec4 accent = todo ? colour::warn : colour::cyan;
        switch (block.kind)
        {
        case '1':
            ImGui::PushFont(g_fonts.title);
            Wrapped(block.text, colour::white);
            ImGui::PopFont();
            break;
        case '2':
        {
            ImGui::Dummy(ImVec2(0, 6 * s));
            if (block.text.starts_with("Qué hacer"))
            {
                ImGui::PushStyleColor(ImGuiCol_ChildBg, Hex(0xFBBF24, 0.06f));
                ImGui::PushStyleColor(ImGuiCol_Border, Hex(0xFBBF24, 0.35f));
                ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18, 14) * s);
                ImGui::BeginChild("todo", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
                ImGui::PopStyleVar();
                ImGui::PopStyleColor(2);
                todo = true;
            }
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(at + ImVec2(0, 4 * s), at + ImVec2(3 * s, g_fonts.heading->FontSize), U32(todo ? colour::warn : colour::cyan), 2 * s);
            ImGui::SetCursorScreenPos(at + ImVec2(12 * s, 0));
            ImGui::PushFont(g_fonts.heading);
            Wrapped(block.text, todo ? colour::warn : colour::white);
            ImGui::PopFont();
            break;
        }
        case '3':
            ImGui::Dummy(ImVec2(0, 2 * s));
            ImGui::PushFont(g_fonts.bold);
            Wrapped(block.text, Hex(0xA9B6D0));
            ImGui::PopFont();
            break;
        case '-':
        {
            const float indent = (16 + 16 * block.indent) * s;
            ImGui::Indent(indent);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddCircleFilled(at + ImVec2(-9 * s, ImGui::GetTextLineHeight() * 0.5f + 1 * s), 2.5f * s, U32(accent));
            Wrapped(block.text, colour::soft);
            ImGui::Unindent(indent);
            break;
        }
        case 'n':
        {
            const float indent = (4 + 16 * block.indent) * s;
            ImGui::Indent(indent);
            ImGui::PushFont(g_fonts.bold);
            ImGui::TextColored(accent, "%s", block.mark.c_str());
            ImGui::PopFont();
            ImGui::SameLine(0, 8 * s);
            Wrapped(block.text, colour::text);
            ImGui::Unindent(indent);
            ImGui::Dummy(ImVec2(0, 2 * s));
            break;
        }
        case 'c':
        case 't':
            Code(block.text);
            break;
        default:
            Wrapped(block.text, colour::soft);
            break;
        }
    }
    if (todo)
        ImGui::EndChild();
}

static std::string PickPath(HWND owner, bool folder)
{
    std::string result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
        return result;
    DWORD options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (folder ? FOS_PICKFOLDERS : 0));
    if (!folder)
    {
        const COMDLG_FILTERSPEC filters[] = {
            {L"Juegos de Xbox 360 (ISO, XEX, paquetes XBLA)", L"*.iso;*.xex;*"},
            {L"Todos", L"*.*"},
        };
        dialog->SetFileTypes(2, filters);
    }
    if (SUCCEEDED(dialog->Show(owner)))
    {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)))
        {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
            {
                result = Utf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

// The slot name from the game's file or folder name: lowercase letters, digits and _.
static std::string SlotName(const std::string& path)
{
    fs::path p = fs::path(Wide(path));
    std::wstring stem = p.stem().wstring();
    if (_wcsicmp(p.filename().c_str(), L"default.xex") == 0 && p.has_parent_path())
        stem = p.parent_path().filename().wstring();
    std::string slot;
    for (wchar_t c : stem)
    {
        if (c < 128 && iswalnum(c))
            slot += char(towlower(c));
        else if (!slot.empty() && slot.back() != '_')
            slot += '_';
    }
    while (!slot.empty() && slot.back() == '_')
        slot.pop_back();
    return slot.empty() ? "juego" : slot.substr(0, 40);
}

static std::string ReadFile(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    return text.str();
}

static std::wstring Quote(const std::string& value)
{
    std::wstring out = L"\"";
    for (wchar_t c : Wide(value))
    {
        if (c != L'"')
            out += c;
    }
    while (!out.empty() && out.back() == L'\\')
        out.pop_back();
    return out + L"\"";
}

static std::string Clock(double seconds)
{
    const int total = int(seconds), hours = total / 3600, minutes = total / 60 % 60;
    char text[32];
    if (hours)
        snprintf(text, sizeof(text), "%d:%02d:%02d", hours, minutes, total % 60);
    else
        snprintf(text, sizeof(text), "%d:%02d", minutes, total % 60);
    return text;
}

// Segoe UI for the text, Cascadia Mono (or Consolas) for the log; ImGui's own font if they are missing.
static void LoadFonts(ImGuiIO& io)
{
    // Latin-1, and the dashes, quotes, bullet, ellipsis, arrows and ≤ ≥ that the reports use.
    static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2013, 0x2026, 0x2190, 0x2193, 0x2264, 0x2265, 0};
    const auto load = [&](std::initializer_list<const char*> files, float size) -> ImFont* {
        for (const char* file : files)
        {
            const std::string path = std::string("C:\\Windows\\Fonts\\") + file;
            if (fs::exists(path))
                return io.Fonts->AddFontFromFileTTF(path.c_str(), size * g_scale, nullptr, ranges);
        }
        return nullptr;
    };
    g_fonts.body = load({"segoeui.ttf"}, 17.0f);
    if (!g_fonts.body)
    {
        ImFontConfig config;
        config.SizePixels = 15.0f * g_scale;
        g_fonts.body = io.Fonts->AddFontDefault(&config);
    }
    const auto orBody = [](ImFont* font) { return font ? font : g_fonts.body; };
    g_fonts.bold = orBody(load({"seguisb.ttf"}, 17.0f));
    g_fonts.note = orBody(load({"segoeui.ttf"}, 14.0f));
    g_fonts.heading = orBody(load({"seguisb.ttf"}, 21.0f));
    g_fonts.title = orBody(load({"seguisb.ttf"}, 27.0f));
    g_fonts.mono = orBody(load({"CascadiaMono.ttf", "consola.ttf"}, 15.0f));
}

// Rounded panels on the logo's navy, its blue for what is on or chosen.
static void Theme()
{
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(12, 10);
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(10, 8);
    style.ItemInnerSpacing = ImVec2(8, 6);
    style.ScrollbarSize = 12;
    style.WindowRounding = style.PopupRounding = 8;
    style.ChildRounding = 10;
    style.FrameRounding = 7;
    style.ScrollbarRounding = style.GrabRounding = 6;
    style.TabRounding = 7;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = style.PopupBorderSize = style.FrameBorderSize = 1;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 1;
    style.TabBarOverlineSize = 2;
    style.DisabledAlpha = 0.45f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = colour::text;
    c[ImGuiCol_TextDisabled] = colour::faint;
    c[ImGuiCol_WindowBg] = colour::back;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = colour::card;
    c[ImGuiCol_Border] = colour::line;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = colour::field;
    c[ImGuiCol_FrameBgHovered] = Hex(0x0F182C);
    c[ImGuiCol_FrameBgActive] = Hex(0x14203B);
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = Hex(0x223050);
    c[ImGuiCol_ScrollbarGrabHovered] = Hex(0x2C3D63);
    c[ImGuiCol_ScrollbarGrabActive] = colour::blue;
    c[ImGuiCol_CheckMark] = colour::cyan;
    c[ImGuiCol_SliderGrab] = colour::blue;
    c[ImGuiCol_SliderGrabActive] = colour::cyan;
    c[ImGuiCol_Button] = colour::raised;
    c[ImGuiCol_ButtonHovered] = Hex(0x223152);
    c[ImGuiCol_ButtonActive] = Hex(0x2B3E68);
    c[ImGuiCol_Header] = colour::raised;
    c[ImGuiCol_HeaderHovered] = Hex(0x223152);
    c[ImGuiCol_HeaderActive] = Hex(0x2B3E68);
    c[ImGuiCol_Separator] = colour::line;
    c[ImGuiCol_SeparatorHovered] = colour::blue;
    c[ImGuiCol_SeparatorActive] = colour::cyan;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = colour::raised;
    c[ImGuiCol_TabSelected] = colour::well;
    c[ImGuiCol_TabSelectedOverline] = colour::cyan;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = colour::well;
    c[ImGuiCol_TabDimmedSelectedOverline] = colour::cyan;
    c[ImGuiCol_TextSelectedBg] = Hex(0x2F7CFF, 0.35f);
    c[ImGuiCol_NavCursor] = colour::cyan;
    style.ScaleAllSizes(g_scale);
}

// A group of fields on a lighter panel, under its title.
static void BeginCard(const char* title)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, colour::card);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16, 12) * g_scale);
    ImGui::BeginChild(title, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    ImGui::PushFont(g_fonts.bold);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::Dummy(ImVec2(0, 2 * g_scale));
}

static void EndCard()
{
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0, 4 * g_scale));
}

// A field's name, small and dim, just above it; with `right`, the name of a second field `rightAt`
// further on the same row.
static void Label(const char* text, const char* right = nullptr, float rightAt = 0.0f)
{
    ImGui::PushFont(g_fonts.note);
    ImGui::PushStyleColor(ImGuiCol_Text, colour::dim);
    ImGui::TextUnformatted(text);
    if (right)
    {
        ImGui::SameLine(ImGui::GetCursorStartPos().x + rightAt);
        ImGui::TextUnformatted(right);
    }
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4 * g_scale);
}

// A switch for an option: the pill slides on, and what the option does goes under its name.
static void Toggle(const char* name, const char* help, bool* value)
{
    const float s = g_scale, h = 20 * s, w = 36 * s, lineHeight = ImGui::GetTextLineHeight();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton(name, ImVec2(ImGui::GetContentRegionAvail().x, lineHeight + g_fonts.note->FontSize + 3 * s)))
        *value = !*value;
    const bool hot = ImGui::IsItemHovered();
    float& t = *ImGui::GetStateStorage()->GetFloatRef(ImGui::GetItemID(), *value ? 1.0f : 0.0f);
    t += ((*value ? 1.0f : 0.0f) - t) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14.0f);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 a(at.x, at.y + (lineHeight - h) * 0.5f + 2 * s), b = a + ImVec2(w, h);
    draw->AddRectFilled(a, b, U32(Mix(hot ? Hex(0x2A3654) : Hex(0x222C45), colour::blue, t)), h * 0.5f);
    draw->AddCircleFilled(ImVec2(a.x + h * 0.5f + t * (w - h), a.y + h * 0.5f), h * 0.5f - 3 * s, U32(Mix(colour::dim, colour::white, t)));
    const float x = b.x + 12 * s;
    draw->AddText(ImVec2(x, at.y), U32(colour::text), name);
    draw->AddText(g_fonts.note, g_fonts.note->FontSize, ImVec2(x, at.y + lineHeight + 2 * s), U32(colour::dim), help);
}

// A few choices side by side, the chosen one filled.
static void Segmented(const char* id, int* value, const char* const* names, int count, float width)
{
    const float s = g_scale, h = ImGui::GetFrameHeight(), r = ImGui::GetStyle().FrameRounding, part = width / float(count);
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(at, at + ImVec2(width, h), U32(colour::field), r);
    draw->AddRect(at, at + ImVec2(width, h), U32(colour::line), r);
    ImGui::PushID(id);
    for (int i = 0; i < count; ++i)
    {
        const ImVec2 left(at.x + part * float(i), at.y);
        ImGui::SetCursorScreenPos(left);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##part", ImVec2(part, h)))
            *value = i;
        ImGui::PopID();
        const ImVec2 a = left + ImVec2(3, 3) * s, b = left + ImVec2(part, h) - ImVec2(3, 3) * s;
        if (*value == i)
            draw->AddRectFilled(a, b, U32(colour::blue), r - 2 * s);
        else if (ImGui::IsItemHovered())
            draw->AddRectFilled(a, b, U32(colour::raised), r - 2 * s);
        const ImVec2 size = ImGui::CalcTextSize(names[i]);
        draw->AddText(left + ImVec2((part - size.x) * 0.5f, (h - size.y) * 0.5f), U32(*value == i ? colour::white : colour::dim), names[i]);
    }
    ImGui::PopID();
}

// The main button: the logo's cyan-to-blue, glowing under the mouse.
static bool GlowButton(const char* id, const char* text, ImVec2 size, bool enabled)
{
    const float s = g_scale, r = 10 * s;
    ImGui::BeginDisabled(!enabled);
    const ImVec2 a = ImGui::GetCursorScreenPos(), b = a + size;
    const bool pressed = ImGui::InvisibleButton(id, size);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (!enabled)
        draw->AddRectFilled(a, b, ImGui::ColorConvertFloat4ToU32(colour::raised), r);
    else
    {
        const bool hot = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
        if (hot)
        {
            for (int i = 1; i <= 5; ++i)
                draw->AddRect(a - ImVec2(1, 1) * (float(i) * s), b + ImVec2(1, 1) * (float(i) * s), U32(colour::cyan, 0.22f / float(i)), r + float(i) * s, 0, 2 * s);
        }
        const ImU32 left = U32(held ? colour::blue : colour::cyan), right = U32(held ? colour::deep : colour::blue);
        // A rounded gradient: the ends in their colours, the middle a blend between them.
        draw->AddRectFilled(a, ImVec2(a.x + 2 * r, b.y), left, r, ImDrawFlags_RoundCornersLeft);
        draw->AddRectFilled(ImVec2(b.x - 2 * r, a.y), b, right, r, ImDrawFlags_RoundCornersRight);
        draw->AddRectFilledMultiColor(ImVec2(a.x + r, a.y), ImVec2(b.x - r, b.y), left, right, right, left);
    }
    ImFont* font = g_fonts.heading;
    const ImVec2 textSize = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, text);
    draw->AddText(font, font->FontSize, a + (size - textSize) * 0.5f,
                  ImGui::ColorConvertFloat4ToU32(enabled ? colour::white : colour::faint), text);
    ImGui::EndDisabled();
    return pressed;
}

// A rounded badge with a dot, right-aligned at `right`: how the run goes.
static void Pill(ImDrawList* draw, ImVec2 right, const std::string& text, const ImVec4& tone, bool pulse)
{
    const float s = g_scale, pad = 14 * s, dot = 4 * s;
    const ImVec2 size = ImGui::CalcTextSize(text.c_str());
    const ImVec2 b(right.x, right.y + size.y * 0.5f + 7 * s), a(b.x - size.x - 2 * pad - 2 * dot - 9 * s, right.y - size.y * 0.5f - 7 * s);
    const float round = (b.y - a.y) * 0.5f;
    draw->AddRectFilled(a, b, U32(tone, 0.12f), round);
    draw->AddRect(a, b, U32(tone, 0.4f), round);
    const ImVec2 centre(a.x + pad + dot, (a.y + b.y) * 0.5f);
    if (pulse)
    {
        const float t = float(std::fmod(ImGui::GetTime(), 1.4) / 1.4);
        draw->AddCircleFilled(centre, dot + 6 * s * t, U32(tone, 0.5f * (1.0f - t)));
    }
    draw->AddCircleFilled(centre, dot, U32(tone));
    draw->AddText(ImVec2(centre.x + dot + 9 * s, (a.y + b.y - size.y) * 0.5f), U32(Mix(tone, colour::white, 0.55f)), text.c_str());
}

enum class State
{
    idle,
    running,
    done,
    failed,
    stopped
};

// port.py's steps as dots on a line: done, under way, failed, left out or still to come.
static void DrawSteps(int reached, State state, bool noBuild, bool noApk, bool phone)
{
    static const char* const names[] = {"Recompilar", "Ranura", "Shaders", "Compilar", "Arrancar", "Caché", "APK", "Móvil"};
    enum Look
    {
        later,
        skipped,
        done,
        now,
        failed,
        stopped
    };
    const int count = phone ? 8 : 7;
    const float s = g_scale, r = 13 * s, width = ImGui::GetContentRegionAvail().x, margin = 42 * s;
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::Dummy(ImVec2(width, 2 * r + g_fonts.note->FontSize + 14 * s));
    const float gap = (width - 2 * margin) / float(count - 1);
    const auto centre = [&](int i) { return ImVec2(at.x + margin + gap * float(i), at.y + r + 2 * s); };

    Look looks[8];
    for (int i = 0; i < count; ++i)
    {
        const int number = i + 1;
        Look look = (noBuild && number >= 4) || (noApk && number >= 7) ? skipped : later;
        if (state != State::idle && reached)
        {
            if (number < reached)
                look = done;
            else if (number == reached)
                look = state == State::running ? now : state == State::failed ? failed : state == State::stopped ? stopped : done;
            else if (state == State::done)
                look = skipped;
        }
        looks[i] = look;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    for (int i = 0; i + 1 < count; ++i)
    {
        const bool lit = looks[i + 1] >= done;
        draw->AddLine(centre(i) + ImVec2(r + 5 * s, 0), centre(i + 1) - ImVec2(r + 5 * s, 0), U32(lit ? colour::blue : colour::line), 2 * s);
    }
    ImFont* font = g_fonts.note;
    for (int i = 0; i < count; ++i)
    {
        const ImVec2 c = centre(i);
        ImVec4 fill = colour::field, ring = colour::line, ink = colour::dim, label = colour::dim;
        switch (looks[i])
        {
        case done:
            fill = ring = colour::blue;
            label = colour::soft;
            break;
        case now:
        {
            fill = Hex(0x0E1A33);
            ring = ink = colour::cyan;
            label = colour::white;
            const float t = float(std::fmod(ImGui::GetTime(), 1.4) / 1.4);
            draw->AddCircleFilled(c, r + 9 * s * t, U32(colour::cyan, 0.35f * (1.0f - t)));
            break;
        }
        case failed:
            fill = ring = label = colour::bad;
            ink = colour::white;
            break;
        case stopped:
            fill = ring = label = colour::warn;
            ink = Hex(0x1A1300);
            break;
        case skipped:
            ink = label = colour::faint;
            break;
        default:
            break;
        }
        draw->AddCircleFilled(c, r, U32(fill));
        draw->AddCircle(c, r, U32(ring), 0, 2 * s);
        if (looks[i] == done)
        {
            const ImVec2 tick[] = {c + ImVec2(-5.0f, 0.5f) * s, c + ImVec2(-1.5f, 4.0f) * s, c + ImVec2(5.0f, -3.5f) * s};
            draw->AddPolyline(tick, 3, U32(colour::white), 0, 2.2f * s);
        }
        else
        {
            char number[4];
            snprintf(number, sizeof(number), looks[i] == failed ? "!" : "%d", i + 1);
            ImFont* bold = g_fonts.bold;
            const ImVec2 size = bold->CalcTextSizeA(bold->FontSize, FLT_MAX, 0.0f, number);
            draw->AddText(bold, bold->FontSize, c - size * 0.5f, U32(ink), number);
        }
        const ImVec2 size = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.0f, names[i]);
        draw->AddText(font, font->FontSize, ImVec2(c.x - size.x * 0.5f, c.y + r + 6 * s), U32(label), names[i]);
    }
}

// Faint text in the middle of what is left of the panel: what will show there.
static void Empty(const char* text)
{
    const ImVec2 room = ImGui::GetContentRegionAvail(), size = ImGui::CalcTextSize(text);
    ImGui::SetCursorPos(ImGui::GetCursorPos() + ImVec2(std::max(0.0f, (room.x - size.x) * 0.5f), std::max(0.0f, (room.y - size.y) * 0.5f)));
    ImGui::TextColored(colour::faint, "%s", text);
}

// The logo's X for the header (xcompiler-mark.png, built into the exe by xcompiler.rc), scaled by
// WIC to `height` pixels so it stays sharp.
static ID3D11ShaderResourceView* LoadMark(HINSTANCE instance, UINT height, ImVec2& size)
{
    HRSRC found = FindResourceW(instance, MAKEINTRESOURCEW(2), RT_RCDATA);
    if (!found)
        return nullptr;
    BYTE* data = static_cast<BYTE*>(LockResource(LoadResource(instance, found)));
    const DWORD bytes = SizeofResource(instance, found);
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICBitmapScaler* scaler = nullptr;
    IWICFormatConverter* converter = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
        SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromMemory(data, bytes)) &&
        SUCCEEDED(factory->CreateDecoderFromStream(stream, nullptr, WICDecodeMetadataCacheOnLoad, &decoder)) &&
        SUCCEEDED(decoder->GetFrame(0, &frame)) && SUCCEEDED(frame->GetSize(&w, &h)) && h &&
        SUCCEEDED(factory->CreateBitmapScaler(&scaler)) &&
        SUCCEEDED(scaler->Initialize(frame, (w * height + h / 2) / h, height, WICBitmapInterpolationModeHighQualityCubic)) &&
        SUCCEEDED(factory->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(scaler, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) &&
        SUCCEEDED(converter->GetSize(&w, &h)))
    {
        std::vector<BYTE> pixels(size_t(w) * h * 4);
        const D3D11_TEXTURE2D_DESC desc = {w, h, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, {1, 0}, D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        const D3D11_SUBRESOURCE_DATA init = {pixels.data(), w * 4, 0};
        ID3D11Texture2D* texture = nullptr;
        if (SUCCEEDED(converter->CopyPixels(nullptr, w * 4, UINT(pixels.size()), pixels.data())) &&
            SUCCEEDED(g_device->CreateTexture2D(&desc, &init, &texture)))
        {
            g_device->CreateShaderResourceView(texture, nullptr, &view);
            texture->Release();
            size = ImVec2(float(w), float(h));
        }
    }
    for (IUnknown* object : std::initializer_list<IUnknown*>{converter, scaler, frame, decoder, stream, factory})
    {
        if (object)
            object->Release();
    }
    return view;
}

static bool CreateDevice(HWND window)
{
    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferDesc.RefreshRate.Numerator = 60;
    desc.BufferDesc.RefreshRate.Denominator = 1;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                                   &desc, &g_swapChain, &g_device, &level, &g_context);
    if (result == DXGI_ERROR_UNSUPPORTED)
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION,
                                               &desc, &g_swapChain, &g_device, &level, &g_context);
    if (FAILED(result))
        return false;
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    g_device->CreateRenderTargetView(back, nullptr, &g_target);
    back->Release();
    return true;
}

static void ReleaseTarget()
{
    if (g_target)
    {
        g_target->Release();
        g_target = nullptr;
    }
}

static LRESULT WINAPI WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (ImGui_ImplWin32_WndProcHandler(window, message, wparam, lparam))
        return true;
    switch (message)
    {
    case WM_SIZE:
        if (wparam != SIZE_MINIMIZED)
        {
            g_resizeWidth = LOWORD(lparam);
            g_resizeHeight = HIWORD(lparam);
        }
        return 0;
    case WM_GETMINMAXINFO:
        reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = POINT{LONG(980 * g_scale), LONG(660 * g_scale)};
        return 0;
    case WM_DROPFILES:
    {
        wchar_t path[4 * MAX_PATH];
        HDROP drop = reinterpret_cast<HDROP>(wparam);
        if (DragQueryFileW(drop, 0, path, ARRAYSIZE(path)))
            g_dropped = Utf8(path);
        DragFinish(drop);
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wparam & 0xFFF0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ImGui_ImplWin32_EnableDpiAwareness();
    const fs::path root = FindRoot();
    // The tools write UTF-8 and unbuffered, so progress shows as it happens.
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
    SetEnvironmentVariableW(L"PYTHONUNBUFFERED", L"1");

    // The logo (xcompiler.rc): the large icon for Alt+Tab and the taskbar, the small one for the title bar.
    const auto icon = [&](int size) {
        return static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0));
    };
    WNDCLASSEXW windowClass = {sizeof(windowClass), CS_CLASSDC, WindowProc, 0, 0, instance,
                               icon(GetSystemMetrics(SM_CXICON)), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr,
                               L"Xcompiler", icon(GetSystemMetrics(SM_CXSMICON))};
    RegisterClassExW(&windowClass);
    g_scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    // 1280x900, or less on a smaller screen, in the middle of it.
    RECT screen = {0, 0, 1920, 1080};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &screen, 0);
    const int width = std::min(int(1280 * g_scale), int(screen.right - screen.left)),
              height = std::min(int(900 * g_scale), int(screen.bottom - screen.top));
    HWND window = CreateWindowW(windowClass.lpszClassName, L"Xcompiler", WS_OVERLAPPEDWINDOW,
                                screen.left + (screen.right - screen.left - width) / 2, screen.top + (screen.bottom - screen.top - height) / 2,
                                width, height, nullptr, nullptr, instance, nullptr);
    if (!CreateDevice(window))
        return 1;
    // A dark title bar in the header's colour (the colour needs Windows 11).
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    const COLORREF caption = RGB(0x0C, 0x12, 0x20);
    DwmSetWindowAttribute(window, 35 /* DWMWA_CAPTION_COLOR */, &caption, sizeof(caption));
    DragAcceptFiles(window, TRUE);
    ShowWindow(window, SW_SHOWDEFAULT);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    g_scale = ImGui_ImplWin32_GetDpiScaleForHwnd(window);
    LoadFonts(io);
    Theme();
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(g_device, g_context);
    ImVec2 markSize;
    ID3D11ShaderResourceView* mark = LoadMark(instance, UINT(44 * g_scale + 0.5f), markSize);

    char gamePath[1024] = "", slot[64] = "", label[128] = "", workDir[1024] = "";
    int seconds = 300, gpu = 0;
    bool noBuild = false, noApk = false, compare = false, phone = false, follow = true;
    // What the run under way (or the last one) was started with, for the row of steps.
    bool runNoBuild = false, runNoApk = false, runPhone = false, stopRequested = false, selectReport = false;
    State state = State::idle;
    std::string status;
    std::vector<Block> report;
    Log log;
    Run run;
    std::chrono::steady_clock::time_point started, ended;

    const auto loadReport = [&] { report = ParseReport(ReadFile(fs::path(Wide(workDir)) / "report.md")); };
    auto chooseGame = [&](const std::string& path) {
        if (path.empty())
            return;
        snprintf(gamePath, sizeof(gamePath), "%s", path.c_str());
        const std::string name = SlotName(path);
        snprintf(slot, sizeof(slot), "%s", name.c_str());
        fs::path p = fs::path(Wide(path));
        snprintf(label, sizeof(label), "%s", Utf8((_wcsicmp(p.filename().c_str(), L"default.xex") == 0 ? p.parent_path() : p).stem().wstring()).c_str());
        snprintf(workDir, sizeof(workDir), "%s", Utf8((root / "work" / Wide(name)).wstring()).c_str());
        // An earlier run of this game leaves its report: it shows until this one writes a new one.
        loadReport();
        selectReport = !report.empty();
    };
    // A game dropped on the exe or its desktop shortcut comes as the first argument.
    int argc = 0;
    if (LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc))
    {
        if (argc > 1)
            chooseGame(Utf8(argv[1]));
        LocalFree(argv);
    }

    bool done = false;
    while (!done)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;
        if (g_resizeWidth != 0 && g_resizeHeight != 0)
        {
            ReleaseTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            ID3D11Texture2D* back = nullptr;
            g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
            g_device->CreateRenderTargetView(back, nullptr, &g_target);
            back->Release();
        }
        if (!g_dropped.empty())
        {
            if (!run.running)
                chooseGame(g_dropped);
            g_dropped.clear();
        }
        bool grew = log.Update(run);
        if (run.finished.exchange(false))
        {
            grew |= log.Update(run);
            ended = std::chrono::steady_clock::now();
            if (stopRequested)
                state = State::stopped, status = "Detenido. Lo hecho hasta aquí sigue en la carpeta de trabajo.";
            else if (run.exitCode != 0)
                state = State::failed, status = "port.py terminó con código " + std::to_string(run.exitCode) + ": mira el registro y logs/.";
            else if (log.failed)
                state = State::failed, status = "Un paso falló: «Qué hacer ahora», en el informe, dice qué mirar.";
            else
                state = State::done, status = "Terminado. El informe está a la derecha y en la carpeta de trabajo.";
            loadReport();
            selectReport = !report.empty();
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        const float s = g_scale;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGui::Begin("Xcompiler", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                               ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        const bool busy = run.running;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos(), full = ImGui::GetWindowSize();

        // Header: the logo's X, the name, and how the run goes.
        const float header = 76 * s;
        draw->AddRectFilled(origin, origin + ImVec2(full.x, header), U32(colour::panel));
        const ImVec2 glowA(origin.x, origin.y + header - 2 * s), glowM(origin.x + full.x * 0.5f, origin.y + header);
        draw->AddRectFilledMultiColor(glowA, glowM, U32(colour::cyan), U32(colour::deep), U32(colour::deep), U32(colour::cyan));
        draw->AddRectFilledMultiColor(ImVec2(glowM.x, glowA.y), origin + ImVec2(full.x, header), U32(colour::deep), U32(colour::deep, 0.0f),
                                      U32(colour::deep, 0.0f), U32(colour::deep));
        float x = origin.x + 22 * s;
        if (mark)
        {
            const ImVec2 at(x, origin.y + (header - markSize.y) * 0.5f);
            draw->AddImage((ImTextureID)(intptr_t)mark, at, at + markSize);
            x += markSize.x + 14 * s;
        }
        ImFont* title = g_fonts.title;
        const float titleY = origin.y + (header - title->FontSize - g_fonts.note->FontSize - 2 * s) * 0.5f - 1 * s;
        draw->AddText(title, title->FontSize, ImVec2(x, titleY), U32(colour::cyan), "X");
        draw->AddText(title, title->FontSize, ImVec2(x + title->CalcTextSizeA(title->FontSize, FLT_MAX, 0.0f, "X").x, titleY),
                      U32(colour::white), "compiler");
        draw->AddText(g_fonts.note, g_fonts.note->FontSize, ImVec2(x + 1 * s, titleY + title->FontSize + 2 * s), U32(colour::dim),
                      "Ports nativos de Xbox 360 a Android");
        const double elapsed = std::chrono::duration<double>((busy ? std::chrono::steady_clock::now() : ended) - started).count();
        const ImVec2 pillAt(origin.x + full.x - 24 * s, origin.y + header * 0.5f);
        switch (state)
        {
        case State::idle:
            Pill(draw, pillAt, "Listo", colour::dim, false);
            break;
        case State::running:
            Pill(draw, pillAt,
                 (log.step == 0   ? std::string("Empezando")
                  : log.step == 8 ? std::string("Probando en el móvil")
                                  : "Paso " + std::to_string(log.step) + " de 7") + "  ·  " + Clock(elapsed),
                 colour::cyan, true);
            break;
        case State::done:
            Pill(draw, pillAt, "Terminado en " + Clock(elapsed), colour::ok, false);
            break;
        case State::failed:
            Pill(draw, pillAt, "Con fallos  ·  " + Clock(elapsed), colour::bad, false);
            break;
        case State::stopped:
            Pill(draw, pillAt, "Detenido", colour::warn, false);
            break;
        }

        // Left: the game, the options and the button.
        const float sideWidth = 420 * s;
        ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + header));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, colour::panel);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 18) * s);
        ImGui::BeginChild("side", ImVec2(sideWidth, 0), ImGuiChildFlags_AlwaysUseWindowPadding);
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        const float spacing = ImGui::GetStyle().ItemSpacing.x;

        ImGui::BeginDisabled(busy);
        BeginCard("Juego");
        const float half = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        Label("Disco o ejecutable");
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##game", "Arrastra aquí el ISO, XEX o carpeta", gamePath, sizeof(gamePath));
        if (ImGui::Button("Elegir archivo…", ImVec2(half, 0)))
            chooseGame(PickPath(window, false));
        ImGui::SameLine();
        if (ImGui::Button("Elegir carpeta…", ImVec2(half, 0)))
            chooseGame(PickPath(window, true));
        Label("Ranura", "Nombre en el móvil", half + spacing);
        ImGui::SetNextItemWidth(half);
        ImGui::InputTextWithHint("##slot", "nombre_corto", slot, sizeof(slot));
        ImGui::SameLine();
        ImGui::SetNextItemWidth(half);
        ImGui::InputTextWithHint("##label", "Como sale en el móvil", label, sizeof(label));
        Label("Carpeta de trabajo");
        const float pick = ImGui::GetFrameHeight() * 1.5f;
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - pick - spacing);
        ImGui::InputTextWithHint("##work", "work\\<ranura>", workDir, sizeof(workDir));
        ImGui::SameLine();
        if (ImGui::Button("…##work", ImVec2(pick, 0)))
        {
            const std::string picked = PickPath(window, true);
            if (!picked.empty())
                snprintf(workDir, sizeof(workDir), "%s", picked.c_str());
        }
        EndCard();

        BeginCard("Opciones");
        Label("Segundos de prueba", "GPU", half + spacing);
        ImGui::SetNextItemWidth(half);
        ImGui::InputInt("##seconds", &seconds, 30, 120);
        seconds = seconds < 30 ? 30 : seconds > 3600 ? 3600 : seconds;
        ImGui::SameLine();
        const char* gpus[] = {"native", "xenia"};
        const char* gpuNames[] = {"Nativa", "Xenia"};
        Segmented("gpu", &gpu, gpuNames, 2, half);
        ImGui::Dummy(ImVec2(0, 2 * s));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(spacing, 5 * s));
        Toggle("Sin compilar ni arrancar", "Recompila, crea la ranura y saca los shaders", &noBuild);
        Toggle("Sin APK", "Se queda en el arranque del PC", &noApk);
        Toggle("Comparar con Xenia", "Arranca también con la GPU de Xenia y compara", &compare);
        Toggle("Probar en el móvil", "Instala el APK por adb y lo prueba", &phone);
        ImGui::PopStyleVar();
        EndCard();
        ImGui::EndDisabled();

        if (GlowButton("##port", busy ? "Portando…" : "Portar", ImVec2(ImGui::GetContentRegionAvail().x, 48 * s),
                       !busy && gamePath[0] && slot[0] && workDir[0]))
        {
            std::wstring command = L"py -3 -I " + Quote(Utf8((root / "tools" / "port.py").wstring())) + L" " + Quote(gamePath) + L" " +
                                   Quote(workDir) + L" " + Wide(SlotName(slot)) + L" --seconds " + std::to_wstring(seconds) +
                                   L" --gpu " + Wide(gpus[gpu]);
            if (label[0])
                command += L" --label " + Quote(label);
            if (noBuild)
                command += L" --no-build";
            if (noApk)
                command += L" --no-apk";
            if (compare)
                command += L" --compare";
            if (phone)
                command += L" --phone";
            log = Log();
            report.clear();
            runNoBuild = noBuild, runNoApk = noApk, runPhone = phone, stopRequested = false;
            started = ended = std::chrono::steady_clock::now();
            if (run.Start(command, root, fs::path(Wide(workDir)) / "gui.log"))
                state = State::running;
            else
                state = State::failed, status = "No se pudo lanzar Python (py -3).";
        }
        ImGui::Dummy(ImVec2(0, 2 * s));
        const float third = (ImGui::GetContentRegionAvail().x - spacing) * 0.5f;
        ImGui::BeginDisabled(!busy);
        ImGui::PushStyleColor(ImGuiCol_Button, Hex(0xF87171, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Hex(0xF87171, 0.20f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Hex(0xF87171, 0.30f));
        ImGui::PushStyleColor(ImGuiCol_Text, colour::bad);
        if (ImGui::Button("Detener", ImVec2(third, 0)))
        {
            stopRequested = true;
            run.Stop();
        }
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!workDir[0]);
        if (ImGui::Button("Abrir carpeta", ImVec2(third, 0)))
            ShellExecuteW(window, L"explore", Wide(workDir).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, 4 * s));
        ImGui::PushFont(g_fonts.note);
        if (state == State::idle)
            Wrapped("Elige un juego o arrástralo a la ventana. Al terminar, el informe dice qué hacer después.", colour::dim);
        else if (state == State::running)
            Wrapped(log.last.empty() ? std::string("Arrancando port.py…") : "Ahora: " + log.last, colour::cyan);
        else
            Wrapped(status, state == State::done ? colour::ok : state == State::stopped ? colour::warn : colour::bad);
        ImGui::PopFont();
        ImGui::EndChild();
        draw->AddLine(ImVec2(origin.x + sideWidth + 0.5f, origin.y + header), ImVec2(origin.x + sideWidth + 0.5f, origin.y + full.y), U32(colour::line));

        // Right: the steps, then the log and the report.
        ImGui::SameLine(0, 0);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24, 20) * s);
        ImGui::BeginChild("main", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        ImGui::PopStyleVar();
        if (state == State::idle)
            DrawSteps(0, state, noBuild, noApk, phone);
        else
            DrawSteps(log.step, state, runNoBuild, runNoApk, runPhone);
        ImGui::Dummy(ImVec2(0, 4 * s));

        const auto tab = [&](const char* name, ImGuiTabItemFlags flags) {
            ImGui::PushFont(g_fonts.bold);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(16, 8) * s);
            const bool open = ImGui::BeginTabItem(name, nullptr, flags);
            ImGui::PopStyleVar();
            ImGui::PopFont();
            return open;
        };
        const auto beginWell = [&](const char* id, ImVec2 padding) {
            ImGui::PushStyleColor(ImGuiCol_ChildBg, colour::well);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, padding * s);
            ImGui::BeginChild(id, ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
            ImGui::PopStyleVar();
            ImGui::PopStyleColor();
        };
        if (ImGui::BeginTabBar("tabs"))
        {
            if (tab("Registro", 0))
            {
                ImGui::Dummy(ImVec2(0, 2 * s));
                ImGui::Checkbox("Seguir el final", &follow);
                const size_t count = log.lines.size() - (log.lines.back() == log.text.size() ? 1 : 0);
                const std::string info = std::to_string(count) + " líneas  ·  gui.log";
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(info.c_str()).x);
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(colour::faint, "%s", info.c_str());
                beginWell("log", ImVec2(14, 12));
                if (!count)
                    Empty("Lo que va haciendo port.py sale aquí al pulsar Portar.");
                else
                {
                    ImGui::PushFont(g_fonts.mono);
                    ImGuiListClipper clipper;
                    clipper.Begin(int(count));
                    while (clipper.Step())
                    {
                        for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
                        {
                            const std::string_view line = log.Line(size_t(i));
                            ImGui::PushStyleColor(ImGuiCol_Text, LineColour(line));
                            ImGui::TextUnformatted(line.data(), line.data() + line.size());
                            ImGui::PopStyleColor();
                        }
                    }
                    if (follow && (busy || grew))
                        ImGui::SetScrollHereY(1.0f);
                    ImGui::PopFont();
                }
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (tab("Informe", selectReport ? ImGuiTabItemFlags_SetSelected : 0))
            {
                ImGui::Dummy(ImVec2(0, 2 * s));
                const fs::path reportPath = fs::path(Wide(workDir)) / "report.md";
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(colour::faint, "%s", workDir[0] ? Utf8(reportPath.wstring()).c_str() : "report.md");
                const float buttons = ImGui::CalcTextSize("Abrir").x + ImGui::CalcTextSize("Recargar").x + 4 * ImGui::GetStyle().FramePadding.x + spacing;
                ImGui::SameLine();
                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - buttons));
                ImGui::BeginDisabled(!workDir[0]);
                if (ImGui::Button("Abrir"))
                    ShellExecuteW(window, L"open", reportPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                ImGui::SameLine();
                if (ImGui::Button("Recargar"))
                    loadReport();
                ImGui::EndDisabled();
                beginWell("report", ImVec2(22, 18));
                if (report.empty())
                    Empty("Aún no hay informe: port.py lo escribe en la carpeta de trabajo.");
                else
                    DrawReport(report);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            selectReport = false;
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::End();

        ImGui::Render();
        const float clear[4] = {colour::back.x, colour::back.y, colour::back.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    run.Stop();
    run.Wait();
    if (mark)
        mark->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseTarget();
    if (g_swapChain)
        g_swapChain->Release();
    if (g_context)
        g_context->Release();
    if (g_device)
        g_device->Release();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    CoUninitialize();
    return 0;
}
