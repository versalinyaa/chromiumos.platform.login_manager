// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/session_manager_impl.h"

#include <string>

#include <base/basictypes.h>
#include <base/bind.h>
#include <base/callback.h>
#include <base/command_line.h>
#include <base/file_util.h>
#include <base/memory/scoped_ptr.h>
#include <base/message_loop_proxy.h>
#include <base/stl_util.h>
#include <base/string_util.h>
#include <chromeos/cryptohome.h>
#include <chromeos/utility.h>
#include <crypto/scoped_nss_types.h>
#include <dbus/dbus-glib-bindings.h>
#include <dbus/dbus-glib.h>

#include "login_manager/device_local_account_policy_service.h"
#include "login_manager/device_management_backend.pb.h"
#include "login_manager/device_policy_service.h"
#include "login_manager/login_metrics.h"
#include "login_manager/nss_util.h"
#include "login_manager/policy_key.h"
#include "login_manager/policy_service.h"
#include "login_manager/process_manager_service_interface.h"
#include "login_manager/system_utils.h"
#include "login_manager/upstart_signal_emitter.h"
#include "login_manager/user_policy_service_factory.h"

using base::FilePath;
using chromeos::cryptohome::home::kGuestUserName;
using chromeos::cryptohome::home::GetUserPath;
using chromeos::cryptohome::home::SanitizeUserName;

namespace login_manager {  // NOLINT

const char SessionManagerImpl::kDemoUser[] = "demouser@";

const char SessionManagerImpl::kStarted[] = "started";
const char SessionManagerImpl::kStopping[] = "stopping";
const char SessionManagerImpl::kStopped[] = "stopped";

const char SessionManagerImpl::kLoggedInFlag[] =
    "/var/run/session_manager/logged_in";
const char SessionManagerImpl::kResetFile[] =
    "/mnt/stateful_partition/factory_install_reset";

namespace {

const size_t kCookieEntropyBytes = 16;

// A buffer of this size is used to parse the command line to restart a
// process like restarting Chrome for the guest mode.
const int kMaxArgumentsSize = 1024 * 8;

// Buffer this size used to convert glib strings into c++ strings.
const uint32 kMaxGCharBufferSize = 200;

// Constants used in email validation.
const char kEmailSeparator = '@';
const char kLegalCharacters[] =
    "abcdefghijklmnopqrstuvwxyz"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    ".@1234567890-+_";

// The name of the pref that Chrome sets to track who the owner is.
const char kDeviceOwnerPref[] = "cros.device.owner";

// The flag to pass to chrome to open a named socket for testing.
const char kTestingChannelFlag[] = "--testing-channel=NamedTestingInterface:";

}  // namespace

// PolicyService::Completion implementation that forwards the result to a DBus
// invocation context.
class DBusGMethodCompletion : public PolicyService::Completion {
 public:
  // Takes ownership of |context|.
  DBusGMethodCompletion(DBusGMethodInvocation* context);
  virtual ~DBusGMethodCompletion();

  virtual void Success();
  virtual void Failure(const PolicyService::Error& error);

 private:
  DBusGMethodInvocation* context_;

  DISALLOW_COPY_AND_ASSIGN(DBusGMethodCompletion);
};

DBusGMethodCompletion::DBusGMethodCompletion(DBusGMethodInvocation* context)
    : context_(context) {
}

DBusGMethodCompletion::~DBusGMethodCompletion() {
  if (context_) {
    NOTREACHED() << "Unfinished DBUS call!";
    dbus_g_method_return(context_, false);
  }
}

void DBusGMethodCompletion::Success() {
  dbus_g_method_return(context_, true);
  context_ = NULL;
  delete this;
}

void DBusGMethodCompletion::Failure(const PolicyService::Error& error) {
  SystemUtils system;
  system.SetAndSendGError(error.code(), context_, error.message().c_str());
  context_ = NULL;
  delete this;
}

struct SessionManagerImpl::UserSession {
 public:
  UserSession(const std::string& username,
              const std::string& userhash,
              bool is_incognito,
              crypto::ScopedPK11Slot slot,
              const scoped_refptr<PolicyService>& policy_service)
      : username(username),
        userhash(userhash),
        is_incognito(is_incognito),
        slot(slot.Pass()),
        policy_service(policy_service) {
  }
  ~UserSession() {}

  const std::string username;
  const std::string userhash;
  const bool is_incognito;
  crypto::ScopedPK11Slot slot;
  const scoped_refptr<PolicyService> policy_service;
};

SessionManagerImpl::SessionManagerImpl(
    scoped_ptr<UpstartSignalEmitter> emitter,
    ProcessManagerServiceInterface* manager,
    LoginMetrics* metrics,
    NssUtil* nss,
    SystemUtils* utils)
    : session_started_(false),
      session_stopping_(false),
      screen_locked_(false),
      upstart_signal_emitter_(emitter.Pass()),
      manager_(manager),
      login_metrics_(metrics),
      nss_(nss),
      system_(utils) {
  // TODO(ellyjones): http://crosbug.com/6615
  // The intent was to use this cookie to authenticate RPC requests from the
  // browser process kicked off by the session_manager.  This didn't actually
  // work, and so the work was never completed.  I'm not deleting this code,
  // because it could be useful and there's not a lot of harm in keeping it.
  // That said, work will need to be done to make cookie_ available both in
  // the code handling RPCs, and the code that actually runs the browser.
  if (!chromeos::SecureRandomString(kCookieEntropyBytes, &cookie_))
    LOG(FATAL) << "Can't generate auth cookie.";
}

SessionManagerImpl::~SessionManagerImpl() {
  STLDeleteValues(&user_sessions_);
}

void SessionManagerImpl::InjectPolicyServices(
    const scoped_refptr<DevicePolicyService>& device_policy,
    scoped_ptr<UserPolicyServiceFactory> user_policy_factory,
    scoped_ptr<DeviceLocalAccountPolicyService> device_local_account_policy) {
  device_policy_ = device_policy;
  user_policy_factory_ = user_policy_factory.Pass();
  device_local_account_policy_ = device_local_account_policy.Pass();
}

void SessionManagerImpl::AnnounceSessionStoppingIfNeeded() {
  if (session_started_) {
    session_stopping_ = true;
    DLOG(INFO) << "emitting D-Bus signal SessionStateChanged:"
               << SessionManagerImpl::kStopping;
    std::vector<std::string> args;
    args.push_back(SessionManagerImpl::kStopping);
    system_->EmitSignalWithStringArgs(login_manager::kSessionStateChangedSignal,
                                      args);
  }
}

void SessionManagerImpl::AnnounceSessionStopped() {
  DLOG(INFO) << "emitting D-Bus signal SessionStateChanged:"
             << SessionManagerImpl::kStopped;
  std::vector<std::string> args;
  args.push_back(SessionManagerImpl::kStopped);
  system_->EmitSignalWithStringArgs(login_manager::kSessionStateChangedSignal,
                                    args);
}

bool SessionManagerImpl::Initialize() {
  if (device_policy_->Initialize()) {
    device_local_account_policy_->UpdateDeviceSettings(
        device_policy_->GetSettings());
    return true;
  }
  return false;
}

void SessionManagerImpl::Finalize() {
  device_policy_->PersistPolicySync();
  for (UserSessionMap::const_iterator it = user_sessions_.begin();
       it != user_sessions_.end(); ++it) {
    if (it->second)
      it->second->policy_service->PersistPolicySync();
  }
}

gboolean SessionManagerImpl::EmitLoginPromptReady(gboolean* OUT_emitted,
                                                  GError** error) {
  login_metrics_->RecordStats("login-prompt-ready");
  // TODO(derat): Stop emitting this signal once no one's listening for it.
  // Jobs that want to run after we're done booting should wait for
  // login-prompt-visible or boot-complete.
  *OUT_emitted =
      upstart_signal_emitter_->EmitSignal("login-prompt-ready", "", error);
  return *OUT_emitted;
}

gboolean SessionManagerImpl::EmitLoginPromptVisible(GError** error) {
  login_metrics_->RecordStats("login-prompt-visible");
  system_->EmitSignal(login_manager::kLoginPromptVisibleSignal);
  return upstart_signal_emitter_->EmitSignal("login-prompt-visible", "", error);
}

gboolean SessionManagerImpl::EnableChromeTesting(gboolean force_relaunch,
                                                 const gchar** extra_args,
                                                 gchar** OUT_filepath,
                                                 GError** error) {
  // Check to see if we already have Chrome testing enabled.
  bool already_enabled = !chrome_testing_path_.empty();

  if (!already_enabled) {
    FilePath temp_file_path;  // So we don't clobber chrome_testing_path_;
    if (!system_->GetUniqueFilenameInWriteOnlyTempDir(&temp_file_path))
      return FALSE;
    chrome_testing_path_ = temp_file_path;
  }

  *OUT_filepath = g_strdup(chrome_testing_path_.value().c_str());

  if (already_enabled && !force_relaunch)
    return TRUE;

  // Delete testing channel file if it already exists.
  system_->RemoveFile(chrome_testing_path_);

  std::vector<std::string> extra_argument_vector;
  // Create extra argument vector.
  while (*extra_args != NULL) {
    extra_argument_vector.push_back(*extra_args);
    ++extra_args;
  }
  // Add testing channel argument to extra arguments.
  std::string testing_argument = kTestingChannelFlag;
  testing_argument.append(chrome_testing_path_.value());
  extra_argument_vector.push_back(testing_argument);

  manager_->RestartBrowserWithArgs(extra_argument_vector, true);
  return TRUE;
}

gboolean SessionManagerImpl::StartSession(gchar* email_address,
                                          gchar* unique_identifier,
                                          gboolean* OUT_done,
                                          GError** error) {
  // Validate the |email_address|.
  const std::string email_string(
      StringToLowerASCII(GCharToString(email_address)));
  const bool is_incognito = ((email_string == kGuestUserName) ||
                             (email_string == kDemoUser));
  if (!is_incognito && !ValidateEmail(email_string)) {
    const char msg[] = "Provided email address is not valid.  ASCII only.";
    LOG(ERROR) << msg;
    SetGError(error, CHROMEOS_LOGIN_ERROR_INVALID_EMAIL, msg);
    return *OUT_done = FALSE;
  }

  // Check if this user already started a session.
  if (user_sessions_.count(email_string) > 0) {
    const char msg[] = "Provided email address already started a session.";
    LOG(ERROR) << msg;
    SetGError(error, CHROMEOS_LOGIN_ERROR_SESSION_EXISTS, msg);
    return *OUT_done = FALSE;
  }

  // Create a UserSession object for this user.
  scoped_ptr<UserSession> user_session(CreateUserSession(email_string,
                                                         is_incognito,
                                                         error));
  if (!user_session.get())
    return *OUT_done = FALSE;

  // Check whether the current user is the owner, and if so make sure she is
  // whitelisted and has an owner key.
  bool user_is_owner = false;
  PolicyService::Error policy_error;
  if (!device_policy_->CheckAndHandleOwnerLogin(user_session->username,
                                                user_session->slot.get(),
                                                &user_is_owner,
                                                &policy_error)) {
    SetGError(error,
              policy_error.code(),
              policy_error.message().c_str());
    return *OUT_done = FALSE;
  }

  // If all previous sessions were incognito (or no previous sessions exist).
  bool is_first_real_user = AllSessionsAreIncognito() && !is_incognito;

  // Send each user login event to UMA (right before we start session
  // since the metrics library does not log events in guest mode).
  int dev_mode = system_->IsDevMode();
  if (dev_mode > -1)
    login_metrics_->SendLoginUserType(dev_mode, is_incognito, user_is_owner);

  *OUT_done =
      upstart_signal_emitter_->EmitSignal(
          "start-user-session",
          StringPrintf("CHROMEOS_USER=%s", email_string.c_str()),
          error);

  if (*OUT_done) {
    LOG(INFO) << "Starting user session";
    manager_->SetBrowserSessionForUser(email_string, user_session->userhash);
    session_started_ = true;
    user_sessions_[email_string] = user_session.release();
    DLOG(INFO) << "emitting D-Bus signal SessionStateChanged:" << kStarted;
    std::vector<std::string> args;
    args.push_back(kStarted);
    system_->EmitSignalWithStringArgs(login_manager::kSessionStateChangedSignal,
                                      args);
    if (device_policy_->KeyMissing() &&
        !device_policy_->Mitigating() &&
        is_first_real_user) {
      // This is the first sign-in on this unmanaged device.  Take ownership.
      manager_->RunKeyGenerator(email_string);
    }

    // Record that a login has successfully completed on this boot.
    system_->AtomicFileWrite(FilePath(kLoggedInFlag), "1", 1);
  }

  return *OUT_done;
}

gboolean SessionManagerImpl::StopSession(gchar* unique_identifier,
                                         gboolean* OUT_done,
                                         GError** error) {
  LOG(INFO) << "Stopping all sessions";
  // Most calls to StopSession() will log the reason for the call.
  // If you don't see a log message saying the reason for the call, it is
  // likely a DBUS message. See dbus_glib_shim.cc for that call.
  manager_->ScheduleShutdown();
  // TODO(cmasone): re-enable these when we try to enable logout without exiting
  //                the session manager
  // browser_.job->StopSession();
  // user_policy_.reset();
  // session_started_ = false;
  return *OUT_done = TRUE;
}

gboolean SessionManagerImpl::StorePolicy(GArray* policy_blob,
                                         DBusGMethodInvocation* context) {
  int flags = PolicyService::KEY_ROTATE;
  if (!session_started_)
    flags |= PolicyService::KEY_INSTALL_NEW | PolicyService::KEY_CLOBBER;
  return device_policy_->Store(reinterpret_cast<uint8*>(policy_blob->data),
                               policy_blob->len,
                               new DBusGMethodCompletion(context),
                               flags) ? TRUE : FALSE;
}

gboolean SessionManagerImpl::RetrievePolicy(GArray** OUT_policy_blob,
                                            GError** error) {
  std::vector<uint8> policy_data;
  return EncodeRetrievedPolicy(device_policy_->Retrieve(&policy_data),
                               policy_data,
                               OUT_policy_blob,
                               error);
}

gboolean SessionManagerImpl::StorePolicyForUser(
    gchar* user_email,
    GArray* policy_blob,
    DBusGMethodInvocation* context) {
  scoped_refptr<PolicyService> policy_service = GetPolicyService(user_email);
  if (!policy_service) {
    const char msg[] = "Cannot store user policy before session is started.";
    LOG(ERROR) << msg;
    system_->SetAndSendGError(
        CHROMEOS_LOGIN_ERROR_SESSION_EXISTS, context, msg);
    return FALSE;
  }

  const bool status = policy_service->Store(
      reinterpret_cast<uint8*>(policy_blob->data),
      policy_blob->len,
      new DBusGMethodCompletion(context),
      PolicyService::KEY_INSTALL_NEW | PolicyService::KEY_ROTATE);
  return status ? TRUE : FALSE;
}

gboolean SessionManagerImpl::RetrievePolicyForUser(gchar* user_email,
                                                   GArray** OUT_policy_blob,
                                                   GError** error) {
  scoped_refptr<PolicyService> policy_service = GetPolicyService(user_email);
  if (!policy_service) {
    const char msg[] = "Cannot retrieve user policy before session is started.";
    LOG(ERROR) << msg;
    SetGError(error, CHROMEOS_LOGIN_ERROR_SESSION_EXISTS, msg);
    return FALSE;
  }

  std::vector<uint8> policy_data;
  return EncodeRetrievedPolicy(policy_service->Retrieve(&policy_data),
                               policy_data,
                               OUT_policy_blob,
                               error);
}

gboolean SessionManagerImpl::StoreDeviceLocalAccountPolicy(
    gchar* account_id,
    GArray* policy_blob,
    DBusGMethodInvocation* context) {
  return device_local_account_policy_->Store(
      GCharToString(account_id),
      reinterpret_cast<uint8*>(policy_blob->data),
      policy_blob->len,
      new DBusGMethodCompletion(context));
}

gboolean SessionManagerImpl::RetrieveDeviceLocalAccountPolicy(
    gchar* account_id,
    GArray** OUT_policy_blob,
    GError** error) {
  std::vector<uint8> policy_data;
  return EncodeRetrievedPolicy(
      device_local_account_policy_->Retrieve(GCharToString(account_id),
                                             &policy_data),
      policy_data,
      OUT_policy_blob,
      error);
}

gboolean SessionManagerImpl::RetrieveSessionState(gchar** OUT_state) {
  if (!session_started_)
    *OUT_state = g_strdup(kStopped);
  else
    *OUT_state = g_strdup(session_stopping_ ? kStopping : kStarted);
  return TRUE;
}

GHashTable* SessionManagerImpl::RetrieveActiveSessions() {
  GHashTable* to_return = g_hash_table_new(g_str_hash, g_str_equal);
  for (UserSessionMap::const_iterator it = user_sessions_.begin();
       it != user_sessions_.end();
       ++it) {
    if (it->second) {
      // glib doesn't actually touch the data put into tha hash, so removing
      // const is OK here.
      g_hash_table_insert(to_return,
                          const_cast<char*>(it->second->username.c_str()),
                          const_cast<char*>(it->second->userhash.c_str()));
    }
  }
  return to_return;
}

gboolean SessionManagerImpl::LockScreen(GError** error) {
  if (!session_started_) {
    LOG(WARNING) << "Attempt to lock screen outside of user session.";
    return FALSE;
  }
  // If all sessions are incognito, then locking is not allowed.
  if (AllSessionsAreIncognito()) {
    LOG(WARNING) << "Attempt to lock screen during Guest session.";
    return FALSE;
  }
  if (!screen_locked_) {
    screen_locked_ = true;
    system_->EmitSignal(chromium::kLockScreenSignal);
  }
  LOG(INFO) << "LockScreen() method called.";
  return TRUE;
}

gboolean SessionManagerImpl::HandleLockScreenShown(GError** error) {
  LOG(INFO) << "HandleLockScreenShown() method called.";
  system_->EmitSignal(login_manager::kScreenIsLockedSignal);
  return TRUE;
}

gboolean SessionManagerImpl::HandleLockScreenDismissed(GError** error) {
  screen_locked_ = false;
  LOG(INFO) << "HandleLockScreenDismissed() method called.";
  system_->EmitSignal(login_manager::kScreenIsUnlockedSignal);
  return TRUE;
}

gboolean SessionManagerImpl::RestartJob(gint pid,
                                        gchar* arguments,
                                        gboolean* OUT_done,
                                        GError** error) {
  if (!manager_->IsBrowser(static_cast<pid_t>(pid))) {
    *OUT_done = FALSE;
    const char msg[] = "Provided pid is unknown.";
    LOG(ERROR) << msg;
    SetGError(error, CHROMEOS_LOGIN_ERROR_UNKNOWN_PID, msg);
    return FALSE;
  }

  // To ensure no overflow.
  gchar arguments_buffer[kMaxArgumentsSize + 1];
  snprintf(arguments_buffer, sizeof(arguments_buffer), "%s", arguments);
  arguments_buffer[kMaxArgumentsSize] = '\0';

  gchar **argv = NULL;
  gint argc = 0;
  if (!g_shell_parse_argv(arguments, &argc, &argv, error)) {
    LOG(ERROR) << "Could not parse command: " << (*error)->message;
    g_strfreev(argv);
    return false;
  }
  CommandLine new_command_line(argc, argv);
  g_strfreev(argv);

  // To set "logged-in" state for BWSI mode.
  StartSession(const_cast<gchar*>(kGuestUserName), NULL, OUT_done, error);
  manager_->RestartBrowserWithArgs(new_command_line.argv(), false);
  return *OUT_done;
}

gboolean SessionManagerImpl::RestartJobWithAuth(gint pid,
                                                gchar* cookie,
                                                gchar* arguments,
                                                gboolean* OUT_done,
                                                GError** error) {
  // This method isn't filtered - instead, we check for cookie validity.
  if (!IsValidCookie(cookie)) {
    *OUT_done = FALSE;
    const char msg[] = "Invalid auth cookie.";
    LOG(ERROR) << msg;
    SetGError(error, CHROMEOS_LOGIN_ERROR_ILLEGAL_SERVICE, msg);
    return FALSE;
  }
  return RestartJob(pid, arguments, OUT_done, error);
}


gboolean SessionManagerImpl::StartDeviceWipe(gboolean* OUT_done,
                                             GError** error) {
  const FilePath session_path(kLoggedInFlag);
  if (system_->Exists(session_path)) {
    const char msg[] = "A user has already logged in this boot.";
    LOG(ERROR) << msg;
    SetGError(error, CHROMEOS_LOGIN_ERROR_ALREADY_SESSION, msg);
    return FALSE;
  }
  InitiateDeviceWipe();
  if (OUT_done)
    *OUT_done = TRUE;
  return TRUE;
}

gboolean SessionManagerImpl::SetFlagsForUser(gchar* user_email,
                                             const gchar** flags,
                                             GError** error) {
  std::vector<std::string> session_user_flags;
  // Create extra argument vector.
  while (*flags != NULL) {
    session_user_flags.push_back(*flags);
    ++flags;
  }
  manager_->SetFlagsForUser(user_email, session_user_flags);

  return TRUE;
}

void SessionManagerImpl::OnPolicyPersisted(bool success) {
  system_->EmitStatusSignal(login_manager::kPropertyChangeCompleteSignal,
                            success);
  device_local_account_policy_->UpdateDeviceSettings(
      device_policy_->GetSettings());
}

void SessionManagerImpl::OnKeyPersisted(bool success) {
  system_->EmitStatusSignal(login_manager::kOwnerKeySetSignal, success);
}

void SessionManagerImpl::ImportValidateAndStoreGeneratedKey(
    const std::string& username,
    const FilePath& temp_key_file) {
  std::string key;
  file_util::ReadFileToString(temp_key_file, &key);
  PLOG_IF(WARNING, !file_util::Delete(temp_key_file, false))
      << "Can't delete " << temp_key_file.value();
  device_policy_->ValidateAndStoreOwnerKey(
      username,
      key,
      user_sessions_[username]->slot.get());
}

void SessionManagerImpl::InitiateDeviceWipe() {
  const char *contents = "fast safe";
  const FilePath reset_path(kResetFile);
  system_->AtomicFileWrite(reset_path, contents, strlen(contents));
  system_->CallMethodOnPowerManager(power_manager::kRequestRestartMethod);
}

gboolean SessionManagerImpl::EncodeRetrievedPolicy(
    bool success,
    const std::vector<uint8>& policy_data,
    GArray** policy_blob,
    GError** error) {
  if (success) {
    *policy_blob = g_array_sized_new(FALSE, FALSE, sizeof(uint8),
                                     policy_data.size());
    if (!*policy_blob) {
      const char msg[] = "Unable to allocate memory for response.";
      LOG(ERROR) << msg;
      SetGError(error, CHROMEOS_LOGIN_ERROR_DECODE_FAIL, msg);
      return FALSE;
    }
    g_array_append_vals(*policy_blob,
                        vector_as_array(&policy_data), policy_data.size());
    return TRUE;
  }

  const char msg[] = "Failed to retrieve policy data.";
  LOG(ERROR) << msg;
  SetGError(error, CHROMEOS_LOGIN_ERROR_ENCODE_FAIL, msg);
  return FALSE;
}

// static
std::string SessionManagerImpl::GCharToString(const gchar* str) {
  char buffer[kMaxGCharBufferSize + 1];
  int len = snprintf(buffer, sizeof(buffer), "%s", str);
  return std::string(buffer, len);
}

// static
void SessionManagerImpl::SetGError(GError** error,
                                   ChromeOSLoginError code,
                                   const char* message) {
  g_set_error(error, CHROMEOS_LOGIN_ERROR, code, "Login error: %s", message);
}

// static
bool SessionManagerImpl::ValidateEmail(const std::string& email_address) {
  if (email_address.find_first_not_of(kLegalCharacters) != std::string::npos)
    return false;

  size_t at = email_address.find(kEmailSeparator);
  // it has NO @.
  if (at == std::string::npos)
    return false;

  // it has more than one @.
  if (email_address.find(kEmailSeparator, at+1) != std::string::npos)
    return false;

  return true;
}

bool SessionManagerImpl::AllSessionsAreIncognito() {
  size_t incognito_count = 0;
  for (UserSessionMap::const_iterator it = user_sessions_.begin();
       it != user_sessions_.end();
       ++it) {
    if (it->second)
      incognito_count += it->second->is_incognito;
  }
  return incognito_count == user_sessions_.size();
}

bool SessionManagerImpl::IsValidCookie(const char *cookie) {
  size_t len = strlen(cookie) < cookie_.size()
             ? strlen(cookie)
             : cookie_.size();
  return chromeos::SafeMemcmp(cookie, cookie_.data(), len) == 0;
}

SessionManagerImpl::UserSession*
SessionManagerImpl::CreateUserSession(const std::string& username,
                                      bool is_incognito,
                                      GError** error) {
  scoped_refptr<PolicyService> user_policy =
      user_policy_factory_->Create(username);
  if (!user_policy) {
    const char msg[] = "User policy failed to initialize.";
    LOG(ERROR) << msg;
    if (error)
      SetGError(error, CHROMEOS_LOGIN_ERROR_POLICY_INIT_FAIL, msg);
    return NULL;
  }
  crypto::ScopedPK11Slot slot(nss_->OpenUserDB(GetUserPath(username)));
  if (!slot) {
    const char msg[] = "Could not open the current user's NSS database.";
    LOG(ERROR) << msg;
    if (error)
      SetGError(error, CHROMEOS_LOGIN_ERROR_NO_USER_NSSDB, msg);
    return NULL;
  }
  return new SessionManagerImpl::UserSession(username,
                                             SanitizeUserName(username),
                                             is_incognito,
                                             slot.Pass(),
                                             user_policy);
}

scoped_refptr<PolicyService> SessionManagerImpl::GetPolicyService(
    gchar* user_email) {
  const std::string username(StringToLowerASCII(GCharToString(user_email)));
  UserSessionMap::const_iterator it = user_sessions_.find(username);
  return it == user_sessions_.end() ? NULL : it->second->policy_service;
}

}  // namespace login_manage
