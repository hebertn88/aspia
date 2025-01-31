//
// Aspia Project
// Copyright (C) 2020 Dmitry Chapyshev <dmitry@aspia.ru>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program. If not, see <https://www.gnu.org/licenses/>.
//

#include "host/desktop_session_manager.h"

#include "base/location.h"
#include "base/logging.h"
#include "base/task_runner.h"
#include "base/desktop/frame.h"
#include "base/ipc/ipc_channel.h"
#include "host/desktop_session_fake.h"
#include "host/desktop_session_ipc.h"
#include "host/desktop_session_process.h"
#include "host/desktop_session_proxy.h"

namespace host {

DesktopSessionManager::DesktopSessionManager(
    std::shared_ptr<base::TaskRunner> task_runner, DesktopSession::Delegate* delegate)
    : task_runner_(task_runner),
      session_proxy_(std::make_shared<DesktopSessionProxy>()),
      session_attach_timer_(base::WaitableTimer::Type::SINGLE_SHOT, task_runner),
      delegate_(delegate)
{
    LOG(LS_INFO) << "DesktopSessionManager Ctor";
}

DesktopSessionManager::~DesktopSessionManager()
{
    LOG(LS_INFO) << "DesktopSessionManager Dtor";

    state_ = State::STOPPING;
    dettachSession(FROM_HERE);
}

void DesktopSessionManager::attachSession(
    const base::Location& location, base::SessionId session_id)
{
    if (state_ == State::ATTACHED)
        return;

    LOG(LS_INFO) << "Attach session with ID: " << session_id
                 << " (from: " << location.toString() << ")";

    if (state_ == State::STOPPED)
    {
        session_attach_timer_.start(std::chrono::minutes(1), [this]()
        {
            LOG(LS_WARNING) << "Session attach timeout";
            onErrorOccurred();
        });
    }

    state_ = State::STARTING;

    std::u16string channel_id = base::IpcServer::createUniqueId();

    server_ = std::make_unique<base::IpcServer>();
    if (!server_->start(channel_id, this))
    {
        LOG(LS_ERROR) << "Failed to start IPC server";

        onErrorOccurred();
        return;
    }

    std::unique_ptr<DesktopSessionProcess> process =
        DesktopSessionProcess::create(session_id, channel_id);
    if (!process)
    {
        LOG(LS_ERROR) << "Failed to create session process";

        onErrorOccurred();
        return;
    }

    LOG(LS_INFO) << "Desktop session process created";
}

void DesktopSessionManager::dettachSession(const base::Location& location)
{
    if (state_ == State::STOPPED || state_ == State::DETACHED)
    {
        LOG(LS_INFO) << "Session already stopped or dettached (" << static_cast<int>(state_) << ")";
        return;
    }

    LOG(LS_INFO) << "Dettach session (from: " << location.toString() << ")";

    if (state_ != State::STOPPING)
        state_ = State::DETACHED;

    session_attach_timer_.stop();
    session_proxy_->stopAndDettach();
    task_runner_->deleteSoon(std::move(session_));

    LOG(LS_INFO) << "Session process is detached";

    if (state_ == State::STOPPING)
        return;

    session_attach_timer_.start(std::chrono::minutes(1), [this]()
    {
        LOG(LS_ERROR) << "Timeout while waiting for session";
        onErrorOccurred();
    });

    // The real session process has ended. We create a temporary fake session.
    session_ = std::make_unique<DesktopSessionFake>(task_runner_, this);
    session_proxy_->attachAndStart(session_.get());
}

std::shared_ptr<DesktopSessionProxy> DesktopSessionManager::sessionProxy() const
{
    return session_proxy_;
}

void DesktopSessionManager::onNewConnection(std::unique_ptr<base::IpcChannel> channel)
{
    if (DesktopSessionProcess::filePath() != channel->peerFilePath())
    {
        LOG(LS_ERROR) << "An attempt was made to connect from an unknown application";
        return;
    }

    LOG(LS_INFO) << "Session process successfully connected";

    session_attach_timer_.stop();

    if (server_)
    {
        LOG(LS_INFO) << "IPC server already exists. Stop it";
        server_->stop();
        task_runner_->deleteSoon(std::move(server_));
    }

    session_ = std::make_unique<DesktopSessionIpc>(std::move(channel), this);

    state_ = State::ATTACHED;
    session_proxy_->attachAndStart(session_.get());
}

void DesktopSessionManager::onErrorOccurred()
{
    if (state_ == State::STOPPED || state_ == State::STOPPING)
        return;

    state_ = State::STOPPING;
    dettachSession(FROM_HERE);
    state_ = State::STOPPED;
}

void DesktopSessionManager::onDesktopSessionStarted()
{
    delegate_->onDesktopSessionStarted();
}

void DesktopSessionManager::onDesktopSessionStopped()
{
    dettachSession(FROM_HERE);
}

void DesktopSessionManager::onScreenCaptured(
    const base::Frame* frame, const base::MouseCursor* mouse_cursor)
{
    delegate_->onScreenCaptured(frame, mouse_cursor);
}

void DesktopSessionManager::onAudioCaptured(const proto::AudioPacket& audio_packet)
{
    delegate_->onAudioCaptured(audio_packet);
}

void DesktopSessionManager::onScreenListChanged(const proto::ScreenList& list)
{
    delegate_->onScreenListChanged(list);
}

void DesktopSessionManager::onClipboardEvent(const proto::ClipboardEvent& event)
{
    delegate_->onClipboardEvent(event);
}

} // namespace host
