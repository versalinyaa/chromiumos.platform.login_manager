// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LOGIN_MANAGER_NSS_UTIL_H_
#define LOGIN_MANAGER_NSS_UTIL_H_

#include <base/basictypes.h>
#include <base/file_path.h>
#include <vector>

namespace login_manager {

// This class holds the device owner's public key.
//
// If there is an owner key on disk, we will load that key, and deny
// attempts to set a new key programmatically.  If there is no key
// present, we will allow the owner's key to be set programmatically,
// and will persist it to disk upon request.  Attempts to set the key
// before on-disk storage has been checked will be denied.
class OwnerKey {
 public:
  OwnerKey(const FilePath& key_file);
  ~OwnerKey();

  virtual bool HaveCheckedDisk();
  virtual bool IsPopulated();

  // If |key_file_| exists, populate the object with the contents of the file.
  // If the file isn't there, that's ok.
  // Will return false if the file exists and there are errors reading it.
  // If this returns true, call IsPopulated() to tell whether or not data was
  // loaded off of disk.
  virtual bool PopulateFromDiskIfPossible();

  // Load key material from |public_key_der|.
  // We will _deny_ such an attempt if we have not yet checked disk for a key,
  // or if we have already successfully loaded a key from disk.
  virtual bool PopulateFromBuffer(const std::vector<uint8>& public_key_der);

  // Persist |key_| to disk, at |key_file_|.
  // Calling this method before checking for a key on disk is an error.
  // Returns false if |key_file_| already exists, or if there's an error while
  // writing data.
  virtual bool Persist();
 private:
  const FilePath key_file_;
  bool have_checked_disk_;
  std::vector<uint8> key_;

  DISALLOW_COPY_AND_ASSIGN(OwnerKey);
};
}  // namespace login_manager

#endif  // LOGIN_MANAGER_NSS_UTIL_H_
