// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LOGIN_MANAGER_MOCK_KEY_GENERATOR_H_
#define LOGIN_MANAGER_MOCK_KEY_GENERATOR_H_

#include "login_manager/key_generator.h"

#include <string>

#include <gmock/gmock.h>

namespace login_manager {
class ChildJobInterface;
class SessionManagerService;

class MockKeyGenerator : public KeyGenerator {
 public:
  MockKeyGenerator();
  virtual ~MockKeyGenerator();
  MOCK_METHOD2(Start, bool(const std::string&, uid_t));
};
}  // namespace login_manager

#endif  // LOGIN_MANAGER_MOCK_KEY_GENERATOR_H_
