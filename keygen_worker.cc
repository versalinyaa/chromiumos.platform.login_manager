// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "login_manager/keygen_worker.h"

#include <unistd.h>
#include <sys/types.h>

#include <set>
#include <string>

#include <base/basictypes.h>
#include <base/file_path.h>
#include <base/file_util.h>
#include <base/logging.h>
#include <crypto/rsa_private_key.h>
#include <crypto/scoped_nss_types.h>

#include "login_manager/nss_util.h"
#include "login_manager/policy_key.h"
#include "login_manager/system_utils.h"

namespace login_manager {

namespace keygen {

int GenerateKey(const base::FilePath& file_path,
                const base::FilePath& user_homedir,
                NssUtil* nss) {
  PolicyKey key(file_path, nss);
  if (!key.PopulateFromDiskIfPossible())
    LOG(FATAL) << "Corrupted key on disk at " << file_path.value();
  if (key.IsPopulated())
    LOG(FATAL) << "Existing owner key at " << file_path.value();
  FilePath nssdb = user_homedir.Append(nss->GetNssdbSubpath());
  PLOG_IF(FATAL, !file_util::PathExists(nssdb)) << nssdb.value()
                                                << " does not exist!";
  if (!file_util::VerifyPathControlledByUser(file_path.DirName(),
                                             nssdb,
                                             getuid(),
                                             std::set<gid_t>())) {
    PLOG(FATAL) << nssdb.value() << " cannot be used by the user!";
  }
  crypto::ScopedPK11Slot slot(nss->OpenUserDB(user_homedir));
  PLOG_IF(FATAL, !slot) << "Could not open/create user NSS DB at "
                          << nssdb.value();
  LOG(INFO) << "Generating Owner key.";

  scoped_ptr<crypto::RSAPrivateKey> pair(
      nss->GenerateKeyPairForUser(slot.get()));
  if (pair.get()) {
    if (!key.PopulateFromKeypair(pair.get()))
      LOG(FATAL) << "Could not use generated keypair.";
    LOG(INFO) << "Writing Owner key to " << file_path.value();
    return (key.Persist() ? 0 : 1);
  }
  LOG(FATAL) << "Could not generate owner key!";
  return 0;
}

}  // namespace keygen

}  // namespace login_manager
