// Copyright (c) 2011-2015 Ryan Prichard
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to
// deal in the Software without restriction, including without limitation the
// rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
// sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.

#include "Agent.h"

#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <string>
#include <utility>
#include <vector>

#include "../include/winpty_constants.h"

#include "../shared/AgentMsg.h"
#include "../shared/Buffer.h"
#include "../shared/DebugClient.h"
#include "../shared/GenRandom.h"
#include "../shared/StringBuilder.h"
#include "../shared/StringUtil.h"
#include "../shared/WindowsVersion.h"
#include "../shared/WinptyAssert.h"

#include "ConsoleInput.h"
#include "NamedPipe.h"
#include "Scraper.h"
#include "Terminal.h"
#include "Win32ConsoleBuffer.h"

namespace {

static BOOL WINAPI consoleCtrlHandler(DWORD dwCtrlType)
{
    if (dwCtrlType == CTRL_C_EVENT) {
        // Do nothing and claim to have handled the event.
        return TRUE;
    }
    return FALSE;
}

// In versions of the Windows console before Windows 10, the SelectAll and
// Mark commands both run quickly, but Mark changes the cursor position read
// by GetConsoleScreenBufferInfo.  Therefore, use SelectAll to be less
// intrusive.
//
// Starting with the new Windows 10 console, the Mark command no longer moves
// the cursor, and SelectAll uses a lot of CPU time.  Therefore, use Mark.
//
// The Windows 10 legacy-mode console behaves the same way as previous console
// versions, so detect which syscommand to use by testing whether Mark changes
// the cursor position.
static void initConsoleFreezeMethod(
        Win32Console &console, Win32ConsoleBuffer &buffer)
{
    const ConsoleScreenBufferInfo info = buffer.bufferInfo();

    // Make sure the buffer and window aren't 1x1.  (Is that even possible?)
    buffer.resizeBuffer(Coord(
        std::max<int>(2, info.dwSize.X),
        std::max<int>(2, info.dwSize.Y)));
    buffer.moveWindow(SmallRect(0, 0, 2, 2));
    const Coord initialPosition(1, 1);
    buffer.setCursorPosition(initialPosition);

    // Test whether MARK moves the cursor.
    ASSERT(!console.frozen());
    console.setFreezeUsesMark(true);
    console.setFrozen(true);
    const bool useMark = (buffer.cursorPosition() == initialPosition);
    console.setFrozen(false);
    trace("Using %s syscommand to freeze console",
        useMark ? "MARK" : "SELECT_ALL");
    console.setFreezeUsesMark(useMark);
}

static inline WriteBuffer newPacket() {
    WriteBuffer packet;
    packet.putRawValue<uint64_t>(0); // Reserve space for size.
    return packet;
}

static HANDLE duplicateHandle(HANDLE h) {
    HANDLE ret = nullptr;
    if (!DuplicateHandle(
            GetCurrentProcess(), h,
            GetCurrentProcess(), &ret,
            0, FALSE, DUPLICATE_SAME_ACCESS)) {
        ASSERT(false && "DuplicateHandle failed!");
    }
    return ret;
}

// It's safe to truncate a handle from 64-bits to 32-bits, or to sign-extend it
// back to 64-bits.  See the MSDN article, "Interprocess Communication Between
// 32-bit and 64-bit Applications".
// https://msdn.microsoft.com/en-us/library/windows/desktop/aa384203.aspx
static int64_t int64FromHandle(HANDLE h) {
    return static_cast<int64_t>(reinterpret_cast<intptr_t>(h));
}

} // anonymous namespace

Agent::Agent(LPCWSTR controlPipeName,
             uint64_t agentFlags,
             int mouseMode,
             int initialCols,
             int initialRows) :
    m_useConerr(agentFlags & WINPTY_FLAG_CONERR),
    m_plainMode(agentFlags & WINPTY_FLAG_PLAIN_OUTPUT),
    m_mouseMode(mouseMode)
{
    trace("Agent::Agent entered");

    const bool outputColor =
        !m_plainMode || (agentFlags & WINPTY_FLAG_COLOR_ESCAPES);
    const Coord initialSize(initialCols, initialRows);

    auto primaryBuffer = openPrimaryBuffer();
    if (m_useConerr) {
        m_errorBuffer = Win32ConsoleBuffer::createErrorBuffer();
    }

    initConsoleFreezeMethod(m_console, *primaryBuffer);

    m_controlPipe = &connectToControlPipe(controlPipeName);
    m_coninPipe = &createDataServerPipe(false, L"conin");
    m_conoutPipe = &createDataServerPipe(true, L"conout");
    if (m_useConerr) {
        m_conerrPipe = &createDataServerPipe(true, L"conerr");
    }

    // Send an initial response packet to winpty.dll containing pipe names.
    {
        auto setupPacket = newPacket();
        setupPacket.putWString(m_coninPipe->name());
        setupPacket.putWString(m_conoutPipe->name());
        if (m_useConerr) {
            setupPacket.putWString(m_conerrPipe->name());
        }
        writePacket(setupPacket);
    }

    std::unique_ptr<Terminal> primaryTerminal;
    primaryTerminal.reset(new Terminal(*m_conoutPipe,
                                       m_plainMode,
                                       outputColor));
    m_primaryScraper.reset(new Scraper(m_console,
                                       *primaryBuffer,
                                       std::move(primaryTerminal),
                                       initialSize));
    if (m_useConerr) {
        std::unique_ptr<Terminal> errorTerminal;
        errorTerminal.reset(new Terminal(*m_conerrPipe,
                                         m_plainMode,
                                         outputColor));
        m_errorScraper.reset(new Scraper(m_console,
                                         *m_errorBuffer,
                                         std::move(errorTerminal),
                                         initialSize));
    }

    m_console.setTitle(m_currentTitle);

    const HANDLE conin = GetStdHandle(STD_INPUT_HANDLE);
    m_consoleInput.reset(new ConsoleInput(conin, m_mouseMode, *this));

    // Setup Ctrl-C handling.  First restore default handling of Ctrl-C.  This
    // attribute is inherited by child processes.  Then register a custom
    // Ctrl-C handler that does nothing.  The handler will be called when the
    // agent calls GenerateConsoleCtrlEvent.
    SetConsoleCtrlHandler(NULL, FALSE);
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);

    setPollInterval(25);
}

Agent::~Agent()
{
    trace("Agent exiting...");
    agentShutdown();
    if (m_childProcess != NULL) {
        CloseHandle(m_childProcess);
    }
}

// Write a "Device Status Report" command to the terminal.  The terminal will
// reply with a row+col escape sequence.  Presumably, the DSR reply will not
// split a keypress escape sequence, so it should be safe to assume that the
// bytes before it are complete keypresses.
void Agent::sendDsr()
{
    if (!m_plainMode && !m_conoutPipe->isClosed()) {
        m_conoutPipe->write("\x1B[6n");
    }
}

NamedPipe &Agent::connectToControlPipe(LPCWSTR pipeName)
{
    NamedPipe &pipe = createNamedPipe();
    pipe.connectToServer(pipeName, NamedPipe::OpenMode::Duplex);
    pipe.setReadBufferSize(64 * 1024);
    return pipe;
}

// Returns a new server named pipe.  It has not yet been connected.
NamedPipe &Agent::createDataServerPipe(bool write, const wchar_t *kind)
{
    const auto name =
        (WStringBuilder(128)
            << L"\\\\.\\pipe\\winpty-"
            << kind << L'-'
            << GenRandom().uniqueName()).str_moved();
    NamedPipe &pipe = createNamedPipe();
    pipe.openServerPipe(
        name.c_str(),
        write ? NamedPipe::OpenMode::Writing
              : NamedPipe::OpenMode::Reading,
        write ? 8192 : 0,
        write ? 0 : 256);
    if (!write) {
        pipe.setReadBufferSize(64 * 1024);
    }
    return pipe;
}

void Agent::onPipeIo(NamedPipe &namedPipe)
{
    if (&namedPipe == m_conoutPipe || &namedPipe == m_conerrPipe) {
        autoClosePipesForShutdown();
    } else if (&namedPipe == m_coninPipe) {
        pollConinPipe();
    } else if (&namedPipe == m_controlPipe) {
        pollControlPipe();
    }
}

void Agent::pollControlPipe()
{
    if (m_controlPipe->isClosed()) {
        trace("Agent shutting down");
        shutdown();
        return;
    }

    while (true) {
        uint64_t packetSize = 0;
        const auto amt1 =
            m_controlPipe->peek(&packetSize, sizeof(packetSize));
        if (amt1 < sizeof(packetSize)) {
            break;
        }
        ASSERT(packetSize >= sizeof(packetSize) && packetSize <= SIZE_MAX);
        if (m_controlPipe->bytesAvailable() < packetSize) {
            if (m_controlPipe->readBufferSize() < packetSize) {
                m_controlPipe->setReadBufferSize(packetSize);
            }
            break;
        }
        std::vector<char> packetData;
        packetData.resize(packetSize);
        const auto amt2 = m_controlPipe->read(packetData.data(), packetSize);
        ASSERT(amt2 == packetSize);
        try {
            ReadBuffer buffer(std::move(packetData));
            buffer.getRawValue<uint64_t>(); // Discard the size.
            handlePacket(buffer);
        } catch (const ReadBuffer::DecodeError &error) {
            ASSERT(false && "Decode error");
        }
    }
}

void Agent::handlePacket(ReadBuffer &packet)
{
    const int type = packet.getInt32();
    switch (type) {
    case AgentMsg::StartProcess:
        handleStartProcessPacket(packet);
        break;
    case AgentMsg::SetSize:
        // TODO: I think it might make sense to collapse consecutive SetSize
        // messages.  i.e. The terminal process can probably generate SetSize
        // messages faster than they can be processed, and some GUIs might
        // generate a flood of them, so if we can read multiple SetSize packets
        // at once, we can ignore the early ones.
        handleSetSizePacket(packet);
        break;
    default:
        trace("Unrecognized message, id:%d", type);
    }
}

void Agent::writePacket(WriteBuffer &packet)
{
    const auto &bytes = packet.buf();
    packet.replaceRawValue<uint64_t>(0, bytes.size());
    m_controlPipe->write(bytes.data(), bytes.size());
}

void Agent::handleStartProcessPacket(ReadBuffer &packet)
{
    ASSERT(m_childProcess == nullptr);
    ASSERT(!m_closingOutputPipes);

    const uint64_t spawnFlags = packet.getInt64();
    const bool wantProcessHandle = packet.getInt32();
    const bool wantThreadHandle = packet.getInt32();
    const auto program = packet.getWString();
    const auto cmdline = packet.getWString();
    const auto cwd = packet.getWString();
    const auto env = packet.getWString();
    const auto desktop = packet.getWString();
    packet.assertEof();

    // Ensure that all I/O pipes are connected.  At least the output pipes
    // must be connected eventually, or data will back up (and eventually, if
    // it's ever implemented, the console may become frozen indefinitely).
    // Connecting the output pipes late is racy if auto-shutdown is enabled,
    // because the pipe could be closed before it's opened.
    //
    // Return a friendly error back to libwinpty for the sake of programmers
    // integrating with winpty.
    {
        std::wstring pipeList;
        for (NamedPipe *pipe : { m_coninPipe, m_conoutPipe, m_conerrPipe }) {
            if (pipe != nullptr && pipe->isConnecting()) {
                if (!pipeList.empty()) {
                    pipeList.append(L", ");
                }
                pipeList.append(pipe->name());
            }
        }
        if (!pipeList.empty()) {
            auto reply = newPacket();
            reply.putInt32(
                static_cast<int32_t>(StartProcessResult::PipesStillOpen));
            reply.putWString(pipeList);
            writePacket(reply);
            return;
        }
    }

    auto cmdlineV = vectorWithNulFromString(cmdline);
    auto desktopV = vectorWithNulFromString(desktop);
    auto envV = vectorFromString(env);

    LPCWSTR programArg = program.empty() ? nullptr : program.c_str();
    LPWSTR cmdlineArg = cmdline.empty() ? nullptr : cmdlineV.data();
    LPCWSTR cwdArg = cwd.empty() ? nullptr : cwd.c_str();
    LPWSTR envArg = env.empty() ? nullptr : envV.data();

    STARTUPINFOW sui = {};
    PROCESS_INFORMATION pi = {};
    sui.cb = sizeof(sui);
    sui.lpDesktop = desktop.empty() ? nullptr : desktopV.data();
    BOOL inheritHandles = FALSE;
    if (m_useConerr) {
        inheritHandles = TRUE;
        sui.dwFlags |= STARTF_USESTDHANDLES;
        sui.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        sui.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        sui.hStdError = m_errorBuffer->conout();
    }

    const BOOL success =
        CreateProcessW(programArg, cmdlineArg, nullptr, nullptr,
                       /*bInheritHandles=*/inheritHandles,
                       /*dwCreationFlags=*/CREATE_UNICODE_ENVIRONMENT,
                       envArg, cwdArg, &sui, &pi);
    const int lastError = success ? 0 : GetLastError();

    trace("CreateProcess: %s %u",
          (success ? "success" : "fail"),
          static_cast<unsigned int>(pi.dwProcessId));

    auto reply = newPacket();
    if (success) {
        int64_t replyProcess = 0;
        int64_t replyThread = 0;
        if (wantProcessHandle) {
            replyProcess = int64FromHandle(duplicateHandle(pi.hProcess));
        }
        if (wantThreadHandle) {
            replyThread = int64FromHandle(duplicateHandle(pi.hThread));
        }
        CloseHandle(pi.hThread);
        m_childProcess = pi.hProcess;
        m_autoShutdown = (spawnFlags & WINPTY_SPAWN_FLAG_AUTO_SHUTDOWN);
        reply.putInt32(static_cast<int32_t>(StartProcessResult::ProcessCreated));
        reply.putInt64(replyProcess);
        reply.putInt64(replyThread);
    } else {
        reply.putInt32(static_cast<int32_t>(StartProcessResult::CreateProcessFailed));
        reply.putInt32(lastError);
    }
    writePacket(reply);
}

void Agent::handleSetSizePacket(ReadBuffer &packet)
{
    int cols = packet.getInt32();
    int rows = packet.getInt32();
    packet.assertEof();
    resizeWindow(cols, rows);
    auto reply = newPacket();
    writePacket(reply);
}

void Agent::pollConinPipe()
{
    const std::string newData = m_coninPipe->readAllToString();
    if (hasDebugFlag("input_separated_bytes")) {
        // This debug flag is intended to help with testing incomplete escape
        // sequences and multibyte UTF-8 encodings.  (I wonder if the normal
        // code path ought to advance a state machine one byte at a time.)
        for (size_t i = 0; i < newData.size(); ++i) {
            m_consoleInput->writeInput(newData.substr(i, 1));
        }
    } else {
        m_consoleInput->writeInput(newData);
    }
}

void Agent::onPollTimeout()
{
    // Check the mouse input flag so we can output a trace message.
    const bool enableMouseMode = m_consoleInput->updateMouseInputFlags();

    // Give the ConsoleInput object a chance to flush input from an incomplete
    // escape sequence (e.g. pressing ESC).
    m_consoleInput->flushIncompleteEscapeCode();

    const bool shouldScrapeContent = !m_closingOutputPipes;

    // Check if the child process has exited.
    if (m_autoShutdown &&
            m_childProcess != nullptr &&
            WaitForSingleObject(m_childProcess, 0) == WAIT_OBJECT_0) {
        CloseHandle(m_childProcess);
        m_childProcess = nullptr;

        // Close the data socket to signal to the client that the child
        // process has exited.  If there's any data left to send, send it
        // before closing the socket.
        m_closingOutputPipes = true;
    }

    // Scrape for output *after* the above exit-check to ensure that we collect
    // the child process's final output.
    if (shouldScrapeContent) {
        syncConsoleTitle();
        scrapeBuffers();
    }

    // We must ensure that we disable mouse mode before closing the CONOUT
    // pipe, so update the mouse mode here.
    m_primaryScraper->terminal().enableMouseMode(
        enableMouseMode && !m_closingOutputPipes);

    autoClosePipesForShutdown();
}

void Agent::autoClosePipesForShutdown()
{
    if (m_closingOutputPipes) {
        if (!m_conoutPipe->isClosed() &&
                m_conoutPipe->bytesToSend() == 0) {
            trace("Closing CONOUT pipe (auto-shutdown)");
            m_conoutPipe->closePipe();
        }
        if (m_conerrPipe != nullptr &&
                !m_conerrPipe->isClosed() &&
                m_conerrPipe->bytesToSend() == 0) {
            trace("Closing CONERR pipe (auto-shutdown)");
            m_conerrPipe->closePipe();
        }
    }
}

std::unique_ptr<Win32ConsoleBuffer> Agent::openPrimaryBuffer()
{
    // If we're using a separate buffer for stderr, and a program were to
    // activate the stderr buffer, then we could accidentally scrape the same
    // buffer twice.  That probably shouldn't happen in ordinary use, but it
    // can be avoided anyway by using the original console screen buffer in
    // that mode.
    if (!m_useConerr) {
        return Win32ConsoleBuffer::openConout();
    } else {
        return Win32ConsoleBuffer::openStdout();
    }
}

void Agent::resizeWindow(const int cols, const int rows)
{
    if (cols < 1 ||
            cols > MAX_CONSOLE_WIDTH ||
            rows < 1 ||
            rows > BUFFER_LINE_COUNT - 1) {
        trace("resizeWindow: invalid size: cols=%d,rows=%d", cols, rows);
        return;
    }
    Win32Console::FreezeGuard guard(m_console, true);
    const Coord newSize(cols, rows);
    ConsoleScreenBufferInfo info;
    m_primaryScraper->resizeWindow(*openPrimaryBuffer(), newSize, info);
    m_consoleInput->setMouseWindowRect(info.windowRect());
    if (m_errorScraper) {
        m_errorScraper->resizeWindow(*m_errorBuffer, newSize, info);
    }
}

void Agent::scrapeBuffers()
{
    Win32Console::FreezeGuard guard(m_console, true);
    ConsoleScreenBufferInfo info;
    m_primaryScraper->scrapeBuffer(*openPrimaryBuffer(), info);
    m_consoleInput->setMouseWindowRect(info.windowRect());
    if (m_errorScraper) {
        m_errorScraper->scrapeBuffer(*m_errorBuffer, info);
    }
}

void Agent::syncConsoleTitle()
{
    std::wstring newTitle = m_console.title();
    if (newTitle != m_currentTitle) {
        std::string command = std::string("\x1b]0;") +
                utf8FromWide(newTitle) + "\x07";
        m_conoutPipe->write(command.c_str());
        m_currentTitle = newTitle;
    }
}
