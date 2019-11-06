/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

/**
 * The gd API exported to the legacy api
 */
namespace bluetooth {
namespace shim {

struct IDiscoverability {
  virtual void StartGeneralDiscoverability() = 0;
  virtual void StartLimitedDiscoverability() = 0;
  virtual void StopDiscoverability() = 0;

  virtual bool IsGeneralDiscoverabilityEnabled() const = 0;
  virtual bool IsLimitedDiscoverabilityEnabled() const = 0;

  virtual ~IDiscoverability() {}
};

}  // namespace shim
}  // namespace bluetooth
