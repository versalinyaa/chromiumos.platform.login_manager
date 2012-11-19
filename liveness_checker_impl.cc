// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/liveness_checker_impl.h"

#include <base/basictypes.h>
#include <base/bind.h>
#include <base/callback.h>
#include <base/cancelable_callback.h>
#include <base/compiler_specific.h>
#include <base/memory/ref_counted.h>
#include <base/memory/weak_ptr.h>
#include <base/message_loop_proxy.h>
#include <base/time.h>
#include <chromeos/dbus/service_constants.h>

#include "login_manager/session_manager_service.h"
#include "login_manager/system_utils.h"

namespace login_manager {
namespace {
const uint32 kLivenessCheckIntervalSeconds = 60;
}

LivenessCheckerImpl::LivenessCheckerImpl(
    SessionManagerService* manager,
    SystemUtils* utils,
    const scoped_refptr<base::MessageLoopProxy>& loop,
    bool enable_aborting)
    : manager_(manager),
      system_(utils),
      loop_proxy_(loop),
      enable_aborting_(enable_aborting),
      outstanding_liveness_ping_(false),
      ALLOW_THIS_IN_INITIALIZER_LIST(weak_ptr_factory_(this)) {
}

LivenessCheckerImpl::~LivenessCheckerImpl() {
  Stop();
}

void LivenessCheckerImpl::Start() {
  Stop();  // To be certain.
  outstanding_liveness_ping_ = false;
  liveness_check_.Reset(
      base::Bind(&LivenessCheckerImpl::CheckAndSendLivenessPing,
                 weak_ptr_factory_.GetWeakPtr(),
                 kLivenessCheckIntervalSeconds));
  loop_proxy_->PostDelayedTask(
      FROM_HERE,
      liveness_check_.callback(),
      base::TimeDelta::FromSeconds(kLivenessCheckIntervalSeconds));
}

void LivenessCheckerImpl::HandleLivenessConfirmed() {
  LOG(INFO) << "Browser liveness confirmed.";
  outstanding_liveness_ping_ = false;
}

void LivenessCheckerImpl::Stop() {
  weak_ptr_factory_.InvalidateWeakPtrs();
  liveness_check_.Cancel();
}

bool LivenessCheckerImpl::IsRunning() {
  return !liveness_check_.IsCancelled();
}

void LivenessCheckerImpl::CheckAndSendLivenessPing(uint32 interval_seconds) {
  // If there's an un-acked ping, the browser needs to be taken down.
  if (outstanding_liveness_ping_) {
    LOG(WARNING) << "Browser hang detected!";
    if (enable_aborting_) {
      LOG(WARNING) << "Aborting browser process.";
      manager_->AbortBrowser();
      // HandleChildExit() will reap the process and restart if needed.
      Stop();
      return;
    }
  }

  LOG(INFO) << "Sending a liveness ping to the browser.";
  outstanding_liveness_ping_ = true;
  system_->SendSignalToChromium(chromium::kLivenessRequestedSignal, NULL);
  LOG(INFO) << "Scheduling a liveness check in " << interval_seconds << "s.";
  liveness_check_.Reset(
      base::Bind(&LivenessCheckerImpl::CheckAndSendLivenessPing,
                 weak_ptr_factory_.GetWeakPtr(), interval_seconds));
  loop_proxy_->PostDelayedTask(
      FROM_HERE,
      liveness_check_.callback(),
      base::TimeDelta::FromSeconds(interval_seconds));
}

}  // namespace login_manager