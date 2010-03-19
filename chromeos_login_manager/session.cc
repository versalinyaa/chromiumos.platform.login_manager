// Copyright (c) 2009 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This is a standalone driver for PamClient.

#include "base/logging.h"
#include "pam_client.h"
#include <errno.h>
#include <string.h>

int do_pam_stuff(chromeos::PamClient& pam,
		 const char* username,
		 const char* password) {
  if (pam.GetLastPamResult() != PAM_SUCCESS) {
    LOG(ERROR) << "Couldn't init pam lib: " << pam.GetLastPamResult() << "\n";
    return 1;
  }

  if (!pam.Authenticate(username, password)) {
    LOG(ERROR) << "Couldn't authenticate: " << pam.GetLastPamResult() << "\n";
    return 1;
  }

  LOG(INFO) << "authenticated";
  if (!pam.StartSession()) {
    LOG(ERROR) << "Couldn't start session: " << pam.GetLastPamResult() << "\n";
    return 1;
  }
  return 0;
}

int main(int argc, char* argv[]) {
  if (argc != 3) {
    LOG(ERROR) << "Usage: " << argv[0] << " <username> <password>\n";
    return 1;
  }
  int password_len = strlen(argv[2]);
  char* password = new char[password_len + 1];
  strncpy(password, argv[2], password_len);
  password[password_len] = 0;
  // TODO SecureMemset; maybe get that function into our base library?
  memset(argv[2], 0, password_len);

  chromeos::PamClient pam;
  pam.Init("slim");
  int return_value = do_pam_stuff(pam, argv[1], password);

  // TODO SecureMemset; maybe get that function into our base library?
  memset(password, 0, password_len);
  delete [] password;
  return return_value;
}