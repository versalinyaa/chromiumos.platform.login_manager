// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LOGIN_MANAGER_FILE_CHECKER_H_
#define LOGIN_MANAGER_FILE_CHECKER_H_

#include <base/file_path.h>

namespace login_manager {
class FileChecker {
 public:
  explicit FileChecker(const FilePath& filename);
  virtual ~FileChecker();

  virtual bool exists();

 private:
  const FilePath filename_;

  DISALLOW_COPY_AND_ASSIGN(FileChecker);
};

}  // namespace login_manager

#endif  // LOGIN_MANAGER_FILE_CHECKER_H_
