/*
 * Copyright (C) 2014 The Android Open Source Project
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

#ifndef ART_RUNTIME_OAT_FILE_ASSISTANT_H_
#define ART_RUNTIME_OAT_FILE_ASSISTANT_H_

#include <cstdint>
#include <memory>
#include <sstream>
#include <string>

#include "base/compiler_filter.h"
#include "arch/instruction_set.h"
#include "base/os.h"
#include "base/scoped_flock.h"
#include "base/unix_file/fd_file.h"
#include "class_loader_context.h"
#include "oat_file.h"

namespace art {

namespace gc {
namespace space {
class ImageSpace;
}  // namespace space
}  // namespace gc

// Class for assisting with oat file management.
//
// This class collects common utilities for determining the status of an oat
// file on the device, updating the oat file, and loading the oat file.
//
// The oat file assistant is intended to be used with dex locations not on the
// boot class path. See the IsInBootClassPath method for a way to check if the
// dex location is in the boot class path.
class OatFileAssistant {
 public:
  enum DexOptNeeded {
    // No dexopt should (or can) be done to update the apk/jar.
    // Matches Java: dalvik.system.DexFile.NO_DEXOPT_NEEDED = 0
    kNoDexOptNeeded = 0,

    // dex2oat should be run to update the apk/jar from scratch.
    // Matches Java: dalvik.system.DexFile.DEX2OAT_FROM_SCRATCH = 1
    kDex2OatFromScratch = 1,

    // dex2oat should be run to update the apk/jar because the existing code
    // is out of date with respect to the boot image.
    // Matches Java: dalvik.system.DexFile.DEX2OAT_FOR_BOOT_IMAGE
    kDex2OatForBootImage = 2,

    // dex2oat should be run to update the apk/jar because the existing code
    // is out of date with respect to the target compiler filter.
    // Matches Java: dalvik.system.DexFile.DEX2OAT_FOR_FILTER
    kDex2OatForFilter = 3,
  };

  enum OatStatus {
    // kOatCannotOpen - The oat file cannot be opened, because it does not
    // exist, is unreadable, or otherwise corrupted.
    kOatCannotOpen,

    // kOatDexOutOfDate - The oat file is out of date with respect to the dex file.
    kOatDexOutOfDate,

    // kOatBootImageOutOfDate - The oat file is up to date with respect to the
    // dex file, but is out of date with respect to the boot image.
    kOatBootImageOutOfDate,

    // kOatUpToDate - The oat file is completely up to date with respect to
    // the dex file and boot image.
    kOatUpToDate,
  };

  // Constructs an OatFileAssistant object to assist the oat file
  // corresponding to the given dex location with the target instruction set.
  //
  // The dex_location must not be null and should remain available and
  // unchanged for the duration of the lifetime of the OatFileAssistant object.
  // Typically the dex_location is the absolute path to the original,
  // un-optimized dex file.
  //
  // Note: Currently the dex_location must have an extension.
  // TODO: Relax this restriction?
  //
  // The isa should be either the 32 bit or 64 bit variant for the current
  // device. For example, on an arm device, use arm or arm64. An oat file can
  // be loaded executable only if the ISA matches the current runtime.
  //
  // load_executable should be true if the caller intends to try and load
  // executable code for this dex location.
  //
  // only_load_system_executable should be true if the caller intends to have
  // only oat files from /system loaded executable.
  OatFileAssistant(const char* dex_location,
                   const InstructionSet isa,
                   bool load_executable,
                   bool only_load_system_executable = false);

  // Similar to this(const char*, const InstructionSet, bool), however, if a valid zip_fd is
  // provided, vdex, oat, and zip files will be read from vdex_fd, oat_fd and zip_fd respectively.
  // Otherwise, dex_location will be used to construct necessary filenames.
  OatFileAssistant(const char* dex_location,
                   const InstructionSet isa,
                   bool load_executable,
                   bool only_load_system_executable,
                   int vdex_fd,
                   int oat_fd,
                   int zip_fd);

  // Returns true if the dex location refers to an element of the boot class
  // path.
  bool IsInBootClassPath();

  // Return what action needs to be taken to produce up-to-date code for this
  // dex location. If "downgrade" is set to false, it verifies if the current
  // compiler filter is at least as good as an oat file generated with the
  // given compiler filter otherwise, if its set to true, it checks whether
  // the oat file generated with the target filter will be downgraded as
  // compared to the current state. For example, if the current compiler filter is
  // quicken, and target filter is verify, it will recommend to dexopt, while
  // if the target filter is speed profile, it will recommend to keep it in its
  // current state.
  // profile_changed should be true to indicate the profile has recently changed
  // for this dex location.
  // If the purpose of the dexopt is to downgrade the compiler filter,
  // set downgrade to true.
  // Returns a positive status code if the status refers to the oat file in
  // the oat location. Returns a negative status code if the status refers to
  // the oat file in the odex location.
  int GetDexOptNeeded(CompilerFilter::Filter target_compiler_filter,
                      ClassLoaderContext* context,
                      const std::vector<int>& context_fds,
                      bool profile_changed = false,
                      bool downgrade = false);

  // Returns true if there is up-to-date code for this dex location,
  // irrespective of the compiler filter of the up-to-date code.
  bool IsUpToDate();

  // Returns an oat file that can be used for loading dex files.
  // Returns null if no suitable oat file was found.
  //
  // After this call, no other methods of the OatFileAssistant should be
  // called, because access to the loaded oat file has been taken away from
  // the OatFileAssistant object.
  std::unique_ptr<OatFile> GetBestOatFile();

  // Returns a human readable description of the status of the code for the
  // dex file. The returned description is for debugging purposes only.
  std::string GetStatusDump();

  // Computes the optimization status of the given dex file. The result is
  // returned via the two output parameters.
  //   - out_odex_location: the location of the (best) odex that will be used
  //        for loading. See GetBestInfo().
  //   - out_compilation_filter: the level of optimizations (compiler filter)
  //   - out_compilation_reason: the optimization reason. The reason might
  //        be "unknown" if the compiler artifacts were not annotated during optimizations.
  //   - out_odex_status: a human readable refined status of the validity of the odex file.
  //        E.g. up-to-date, boot-image-more-recent, apk-more-recent.
  //
  // This method will try to mimic the runtime effect of loading the dex file.
  // For example, if there is no usable oat file, the compiler filter will be set
  // to "run-from-apk".
  void GetOptimizationStatus(std::string* out_odex_location,
                             std::string* out_compilation_filter,
                             std::string* out_compilation_reason,
                             std::string* out_odex_status);

  static void GetOptimizationStatus(const std::string& filename,
                                    InstructionSet isa,
                                    std::string* out_compilation_filter,
                                    std::string* out_compilation_reason);

  // Open and returns an image space associated with the oat file.
  static std::unique_ptr<gc::space::ImageSpace> OpenImageSpace(const OatFile* oat_file);

  // Loads the dex files in the given oat file for the given dex location.
  // The oat file should be up to date for the given dex location.
  // This loads multiple dex files in the case of multidex.
  // Returns an empty vector if no dex files for that location could be loaded
  // from the oat file.
  //
  // The caller is responsible for freeing the dex_files returned, if any. The
  // dex_files will only remain valid as long as the oat_file is valid.
  static std::vector<std::unique_ptr<const DexFile>> LoadDexFiles(
      const OatFile& oat_file, const char* dex_location);

  // Same as `std::vector<std::unique_ptr<const DexFile>> LoadDexFiles(...)` with the difference:
  //   - puts the dex files in the given vector
  //   - returns whether or not all dex files were successfully opened
  static bool LoadDexFiles(const OatFile& oat_file,
                           const std::string& dex_location,
                           std::vector<std::unique_ptr<const DexFile>>* out_dex_files);

  // Returns whether this is an apk/zip wit a classes.dex entry.
  bool HasDexFiles();

  // If the dex file has been installed with a compiled oat file alongside
  // it, the compiled oat file will have the extension .odex, and is referred
  // to as the odex file. It is called odex for legacy reasons; the file is
  // really an oat file. The odex file will often, but not always, have a
  // patch delta of 0 and need to be relocated before use for the purposes of
  // ASLR. The odex file is treated as if it were read-only.
  //
  // Returns the status of the odex file for the dex location.
  OatStatus OdexFileStatus();

  // When the dex files is compiled on the target device, the oat file is the
  // result. The oat file will have been relocated to some
  // (possibly-out-of-date) offset for ASLR.
  //
  // Returns the status of the oat file for the dex location.
  OatStatus OatFileStatus();

  // Constructs the odex file name for the given dex location.
  // Returns true on success, in which case odex_filename is set to the odex
  // file name.
  // Returns false on error, in which case error_msg describes the error and
  // odex_filename is not changed.
  // Neither odex_filename nor error_msg may be null.
  static bool DexLocationToOdexFilename(const std::string& location,
                                        InstructionSet isa,
                                        std::string* odex_filename,
                                        std::string* error_msg);

  // Constructs the oat file name for the given dex location.
  // Returns true on success, in which case oat_filename is set to the oat
  // file name.
  // Returns false on error, in which case error_msg describes the error and
  // oat_filename is not changed.
  // Neither oat_filename nor error_msg may be null.
  static bool DexLocationToOatFilename(const std::string& location,
                                       InstructionSet isa,
                                       std::string* oat_filename,
                                       std::string* error_msg);

  // Computes the dex location and vdex filename. If the data directory of the process
  // is known, creates an absolute path in that directory and tries to infer path
  // of a corresponding vdex file. Otherwise only creates a basename dex_location
  // from the combined checksums. Returns true if all out-arguments have been set.
  static bool AnonymousDexVdexLocation(const std::vector<const DexFile::Header*>& dex_headers,
                                       InstructionSet isa,
                                       /* out */ std::string* dex_location,
                                       /* out */ std::string* vdex_filename);

  // Returns true if a filename (given as basename) is a name of a vdex for
  // anonymous dex file(s) created by AnonymousDexVdexLocation.
  static bool IsAnonymousVdexBasename(const std::string& basename);

 private:
  class OatFileInfo {
   public:
    // Initially the info is for no file in particular. It will treat the
    // file as out of date until Reset is called with a real filename to use
    // the cache for.
    // Pass true for is_oat_location if the information associated with this
    // OatFileInfo is for the oat location, as opposed to the odex location.
    OatFileInfo(OatFileAssistant* oat_file_assistant, bool is_oat_location);

    bool IsOatLocation();

    const std::string* Filename();

    // Returns true if this oat file can be used for running code. The oat
    // file can be used for running code as long as it is not out of date with
    // respect to the dex code or boot image. An oat file that is out of date
    // with respect to relocation is considered useable, because it's possible
    // to interpret the dex code rather than run the unrelocated compiled
    // code.
    bool IsUseable();

    // Returns the status of this oat file.
    OatStatus Status();

    // Return the DexOptNeeded value for this oat file with respect to the
    // given target_compilation_filter.
    // profile_changed should be true to indicate the profile has recently
    // changed for this dex location.
    // downgrade should be true if the purpose of dexopt is to downgrade the
    // compiler filter.
    DexOptNeeded GetDexOptNeeded(CompilerFilter::Filter target_compiler_filter,
                                 ClassLoaderContext* context,
                                 const std::vector<int>& context_fds,
                                 bool profile_changed,
                                 bool downgrade);

    // Returns the loaded file.
    // Loads the file if needed. Returns null if the file failed to load.
    // The caller shouldn't clean up or free the returned pointer.
    const OatFile* GetFile();

    // Returns true if the file is opened executable.
    bool IsExecutable();

    // Clear any cached information about the file that depends on the
    // contents of the file. This does not reset the provided filename.
    void Reset();

    // Clear any cached information and switch to getting info about the oat
    // file with the given filename.
    void Reset(const std::string& filename,
               bool use_fd,
               int zip_fd = -1,
               int vdex_fd = -1,
               int oat_fd = -1);

    // Release the loaded oat file for runtime use.
    // Returns null if the oat file hasn't been loaded or is out of date.
    // Ensures the returned file is not loaded executable if it has unuseable
    // compiled code.
    //
    // After this call, no other methods of the OatFileInfo should be
    // called, because access to the loaded oat file has been taken away from
    // the OatFileInfo object.
    std::unique_ptr<OatFile> ReleaseFileForUse();

   private:
    // Returns true if the compiler filter used to generate the file is at
    // least as good as the given target filter. profile_changed should be
    // true to indicate the profile has recently changed for this dex
    // location.
    // downgrade should be true if the purpose of dexopt is to downgrade the
    // compiler filter.
    bool CompilerFilterIsOkay(CompilerFilter::Filter target, bool profile_changed, bool downgrade);

    bool ClassLoaderContextIsOkay(ClassLoaderContext* context, const std::vector<int>& context_fds);

    // Release the loaded oat file.
    // Returns null if the oat file hasn't been loaded.
    //
    // After this call, no other methods of the OatFileInfo should be
    // called, because access to the loaded oat file has been taken away from
    // the OatFileInfo object.
    std::unique_ptr<OatFile> ReleaseFile();

    OatFileAssistant* oat_file_assistant_;
    const bool is_oat_location_;

    bool filename_provided_ = false;
    std::string filename_;

    int zip_fd_ = -1;
    int oat_fd_ = -1;
    int vdex_fd_ = -1;
    bool use_fd_ = false;

    bool load_attempted_ = false;
    std::unique_ptr<OatFile> file_;

    bool status_attempted_ = false;
    OatStatus status_ = OatStatus::kOatCannotOpen;

    // For debugging only.
    // If this flag is set, the file has been released to the user and the
    // OatFileInfo object is in a bad state and should no longer be used.
    bool file_released_ = false;
  };

  // Return info for the best oat file.
  OatFileInfo& GetBestInfo();

  // Returns true when vdex/oat/odex files should be read from file descriptors.
  // The method checks the value of zip_fd_, and if the value is valid, returns
  // true. This is required to have a deterministic behavior around how different
  // files are being read.
  bool UseFdToReadFiles();

  // Returns true if the dex checksums in the given vdex file are up to date
  // with respect to the dex location. If the dex checksums are not up to
  // date, error_msg is updated with a message describing the problem.
  bool DexChecksumUpToDate(const VdexFile& file, std::string* error_msg);

  // Returns true if the dex checksums in the given oat file are up to date
  // with respect to the dex location. If the dex checksums are not up to
  // date, error_msg is updated with a message describing the problem.
  bool DexChecksumUpToDate(const OatFile& file, std::string* error_msg);

  // Return the status for a given opened oat file with respect to the dex
  // location.
  OatStatus GivenOatFileStatus(const OatFile& file);

  // Gets the dex checksums required for an up-to-date oat file.
  // Returns cached_required_dex_checksums if the required checksums were
  // located. Returns null if the required checksums were not found.  The
  // caller shouldn't clean up or free the returned pointer.  This sets the
  // has_original_dex_files_ field to true if the checksums were found for the
  // dex_location_ dex file.
  const std::vector<uint32_t>* GetRequiredDexChecksums();

  // Validates the boot class path checksum of an OatFile.
  bool ValidateBootClassPathChecksums(const OatFile& oat_file);

  std::string dex_location_;

  // Whether or not the parent directory of the dex file is writable.
  bool dex_parent_writable_ = false;

  // In a properly constructed OatFileAssistant object, isa_ should be either
  // the 32 or 64 bit variant for the current device.
  const InstructionSet isa_ = InstructionSet::kNone;

  // Whether we will attempt to load oat files executable.
  bool load_executable_ = false;

  // Whether only oat files on /system are loaded executable.
  const bool only_load_system_executable_ = false;
  // Whether the potential zip file only contains uncompressed dex.
  // Will be set during GetRequiredDexChecksums.
  bool zip_file_only_contains_uncompressed_dex_ = true;

  // Cached value of the required dex checksums.
  // This should be accessed only by the GetRequiredDexChecksums() method.
  std::vector<uint32_t> cached_required_dex_checksums_;
  bool required_dex_checksums_attempted_ = false;
  bool required_dex_checksums_found_;
  bool has_original_dex_files_;

  OatFileInfo odex_;
  OatFileInfo oat_;

  // File descriptor corresponding to apk, dex file, or zip.
  int zip_fd_;

  std::string cached_boot_class_path_;
  std::string cached_boot_class_path_checksums_;

  friend class OatFileAssistantTest;

  DISALLOW_COPY_AND_ASSIGN(OatFileAssistant);
};

std::ostream& operator << (std::ostream& stream, const OatFileAssistant::OatStatus status);

}  // namespace art

#endif  // ART_RUNTIME_OAT_FILE_ASSISTANT_H_
