/*  monero-update - An downloaded/checker updater for Monero
 *
 *  Copyright (c) 2019, The Monero Project
 *
 *  All rights reserved.
 *
 *  monero-update is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  monero-update is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with monero-update.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <unistd.h>
#include <random>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <gpgme.h>
#include <QDir>
#include <QStringList>
#include "misc_log_ex.h"
#include "reg_exp_definer.h"
#include "file_io_utils.h"
#include "common/threadpool.h"
#include "common/dns_utils.h"
#include "common/vercmp.h"
#include "common/updates.h"
#include "common/download.h"
#include "common/sha256sum.h"
#include "pubkeys.h"
#include "updater.h"

#define MIN_GITIAN_SIGS 2

void set_strict_default_file_permissions(bool strict)
{
#if defined(__MINGW32__) || defined(__MINGW__)
  // no clue about the odd one out
#else
  mode_t mode = strict ? 077 : 0;
  umask(mode);
#endif
}

static std::string detect_build_tag(void)
{
  std::string cpuinfo;

#if defined _WIN64
  return "win-x64";
#endif

#if defined _WIN32
  return "win-x86";
#endif

#if defined __FreeBSD__ || defined __FreeBSD_kernel__
  return "freebsd";
#endif

#if defined __APPLE__
  return "mac-x64";
#endif

#if defined __linux__ && defined __aarch64__
    return "linux-armv8";
#endif

#if defined __linux__ && defined __arm__
  return "linux-armv7";
#endif

#if defined __linux__ && defined __i386__
  return "linux-x86";
#endif

#if defined __linux__ && defined __x86_64__
  return "linux-x64";
#endif

  return "source";
}

static const std::map<std::string, std::string> dnssec_to_gitian = {
  std::make_pair("linux-x64", "x86_64-linux-gnu"),
  std::make_pair("linux-x32", "i686-linux-gnu"),
  std::make_pair("win-x64", "x86_64-w64-mingw32"),
  std::make_pair("win-x32", "i686-w64-mingw32"),
  std::make_pair("freebsd", "x86_64-unknown-freebsd"),
  std::make_pair("mac-x64", "x86_64-apple-darwin11"),
  std::make_pair("linux-armv7", "arm-linux-gnueabihf"),
  std::make_pair("linux-armv8", "aarch64-linux-gnu"),
};

static const std::map<std::string, std::string> platform_to_gitian = {
  std::make_pair("mac", "osx"),
};

#ifndef BUILDTAG
#define BUILDTAG "source"
#define SUBDIR "source"
#else
#define SUBDIR "cli"
#endif

#define SOFTWARE "monero"

static std::map<State, std::pair<TriState::tristate_t, const char*>> states = {
  std::make_pair(StateNone, std::make_pair(TriState::TriUnknown, "None")),
  std::make_pair(StateInit, std::make_pair(TriState::TriUnknown, "Initializing")),
  std::make_pair(StateQueryDNS, std::make_pair(TriState::TriUnknown, "Querying DNS")),
  std::make_pair(StateDNSFailed, std::make_pair(TriState::TriFalse, "DNS check failed")),
  std::make_pair(StateCheckVersion, std::make_pair(TriState::TriUnknown, "Checking version")),
  std::make_pair(StateUpToDate, std::make_pair(TriState::TriTrue, "We are up to date")),
  std::make_pair(StateBackInTime, std::make_pair(TriState::TriTrue, "Only old versions found")),
  std::make_pair(StateNoUpdateInfoFound, std::make_pair(TriState::TriFalse, "No update information found")),
  std::make_pair(StateDownload, std::make_pair(TriState::TriUnknown, "Downloading update")),
  std::make_pair(StateDownloadFailed, std::make_pair(TriState::TriFalse, "Download failed")),
  std::make_pair(StateCheckHash, std::make_pair(TriState::TriUnknown, "Checking hash")),
  std::make_pair(StateBadHash, std::make_pair(TriState::TriFalse, "Invalid hash")),
  std::make_pair(StateImportPubkeys, std::make_pair(TriState::TriUnknown, "Importing public keys")),
  std::make_pair(StatePubkeyImportFailed, std::make_pair(TriState::TriFalse, "Failed to import public keys")),
  std::make_pair(StateFetchGitianSigs, std::make_pair(TriState::TriUnknown, "Fetching Gitian signatures")),
  std::make_pair(StateVerifyGitianSignatures, std::make_pair(TriState::TriUnknown, "Verifying Gitian signatures")),
  std::make_pair(StateNoGitianSigs, std::make_pair(TriState::TriFalse, "No Gitian signatures found")),
  std::make_pair(StateNotEnoughGitianSigs, std::make_pair(TriState::TriFalse, "Not enough matching Gitian signatures found")),
  std::make_pair(StateBadGitianSigs, std::make_pair(TriState::TriFalse, "At least one Gitian signature was invalid")),
  std::make_pair(StateValidUpdate, std::make_pair(TriState::TriTrue, "Valid update downloaded and verified")),
};

// All four MoneroPulse domains have DNSSEC on and valid
static const std::vector<std::string> dns_urls = {
    "updates.moneropulse.org",
    "updates.moneropulse.net",
    "updates.moneropulse.co",
    "updates.moneropulse.se"
};

static TriState::tristate_t get_state_outcome(State state)
{
  return states[state].first;
}

static const char *get_state_name(State state)
{
  return states[state].second;
}

Updater::Updater(QObject *parent):
  QObject(parent),
  state(StateNone),
  next_state(StateNone),
  dnsValid(TriState::TriUnknown),
  hashValid(TriState::TriUnknown),
  validGitianSigs(0),
  minValidGitianSigs(0),
  totalGitianSigs(0),
  processedGitianSigs(0),

  software(SOFTWARE),
  buildtag(detect_build_tag()),
  current_version(""),

  dns_query_done(false),
  version_check_done(false),
  download_done(false),
  download_success(false),
  gitian_pubkeys_import_done(false),
  gitian_pubkeys_import_success(false),
  gitian_verify_sigs_done(false),
  gitian_verify_sigs_success(false),
  bad_gitian_signature_found(false)
{
  running = true;
  thread = boost::thread([this]() { updater_thread(); } );
  set_state(StateInit);
}

Updater::~Updater()
{
  {
    boost::unique_lock<boost::mutex> lock(mutex);
    running = false;
    cond.notify_one();
  }
  thread.join();
}

void Updater::setDnsValid(tristate_t t)
{
  dnsValid = t;
  emit dnsValidChanged(dnsValid);
}

void Updater::setHashValid(tristate_t t)
{
  hashValid = t;
  emit hashValidChanged(hashValid);
}

void Updater::setValidGitianSigs(uint32_t sigs)
{
  validGitianSigs = sigs;
  emit validGitianSigsChanged(validGitianSigs);
}

void Updater::setMinValidGitianSigs(uint32_t sigs)
{
  minValidGitianSigs = sigs;
  emit minValidGitianSigsChanged(validGitianSigs);
}

void Updater::setProcessedGitianSigs(uint32_t sigs)
{
  processedGitianSigs = sigs;
  emit processedGitianSigsChanged(processedGitianSigs);
}

void Updater::setTotalGitianSigs(uint32_t sigs)
{
  totalGitianSigs = sigs;
  emit totalGitianSigsChanged(totalGitianSigs);
}

void Updater::load_txt_records_from_dns(const std::vector<std::string> &dns_urls, std::vector<dns_query_result_t> &results, std::vector<std::string> &good_records)
{
  boost::unique_lock<boost::mutex> lock(mutex);

  dns_query_done = false;
  setDnsValid(TriState::TriUnknown);
  results.resize(dns_urls.size());
  good_records.clear();

  size_t first_index = (std::default_random_engine(time(NULL) ^ getpid())()) % dns_urls.size();

  add_message("Lookup up DNS TXT records for: " + boost::join(dns_urls, ", "));

  // send all requests in parallel
  tools::threadpool& tpool = tools::threadpool::getInstance();
  tools::threadpool::waiter waiter;
  for (size_t n = 0; n < dns_urls.size(); ++n)
  {
    tpool.submit(&waiter,[n, dns_urls, &results](){
      results[n].records = tools::DNSResolver::instance().get_txt_record(dns_urls[n], results[n].avail, results[n].valid); 
    });
  }
  lock.unlock();
  waiter.wait(&tpool);
  lock.lock();

  size_t cur_index = first_index;
  do
  {
    const std::string &url = dns_urls[cur_index];
    if (!results[cur_index].avail)
    {
      add_message("DNSSEC not available for hostname: " + url + ", skipping.");
    }
    else if (!results[cur_index].valid)
    {
      add_message("DNSSEC validation failed for hostname: " + url + ", skipping.");
    }
    else if (results[cur_index].records.empty())
    {
      add_message("No records for hostname: " + url + ", skipping.");
    }

    cur_index++;
    if (cur_index == dns_urls.size())
    {
      cur_index = 0;
    }
  } while (cur_index != first_index);

  size_t num_valid_records = 0;

  for( const auto& record_set : results)
  {
    if (record_set.avail && record_set.valid && record_set.records.size() != 0)
    {
      num_valid_records++;
    }
  }

  if (num_valid_records < 2)
  {
    add_message("WARNING: no two valid DNS TXT records were received");
    setDnsValid(TriState::TriFalse);
    dns_query_done = true;
    return;
  }

  int good_records_index = -1;
  for (size_t i = 0; i < results.size() - 1; ++i)
  {
    if (!results[i].avail || !results[i].valid || results[i].records.size() == 0) continue;

    for (size_t j = i + 1; j < results.size(); ++j)
    {
      if (tools::dns_utils::dns_records_match(results[i].records, results[j].records))
      {
        good_records_index = i;
        break;
      }
    }
    if (good_records_index >= 0) break;
  }

  if (good_records_index < 0)
  {
    add_message("WARNING: no two DNS TXT records matched");
    setDnsValid(TriState::TriFalse);
    dns_query_done = true;
    return;
  }

  add_message("Found " + std::to_string(num_valid_records) + "/" + std::to_string(dns_urls.size()) + " matching DNSSEC records");
  good_records = results[good_records_index].records;
  setDnsValid(TriState::TriTrue);
  dns_query_done = true;
}

void Updater::process_version(const std::string &software, const std::string &buildtag, const std::vector<std::string> &records)
{
    boost::unique_lock<boost::mutex> lock(mutex);

    version_check_done = false;
    version = "";
    emit versionChanged("");

    bool found = false;

    std::string hash;
    for (const auto& record : records)
    {
      std::vector<std::string> fields;
      add_message("Got record: " + record);
      boost::split(fields, record, boost::is_any_of(":"));
      if (fields.size() != 4)
      {
        add_message("Updates record does not have 4 fields: " + record);
        continue;
      }

      if (software != fields[0] || buildtag != fields[1])
        continue;

      bool alnum = true;
      for (auto c: fields[3])
        if (!isalnum(c))
          alnum = false;
      if (fields[3].size() != 64 && !alnum)
      {
        add_message("Invalid hash: " + fields[3]);
        continue;
      }

      // use highest version
      if (found)
      {
        int cmp = tools::vercmp(version.c_str(), fields[2].c_str());
        if (cmp > 0)
          continue;
        if (cmp == 0 && hash != fields[3])
        {
          add_message("Two matches found for " + software + " version " + version + " on " + buildtag);
          version = "";
          version_check_done = true;
          return;
        }
      }
      version = fields[2];
      hash = fields[3];

      add_message("Found new version " + version + " with hash " + hash);
      found = true;
    }

    if (!version.empty())
    {
      expected_hash = hash;
      emit versionChanged(QString::fromStdString(version));
    }
    version_check_done = true;
}

void Updater::start_download()
{
  boost::unique_lock<boost::mutex> lock(mutex);

  const std::string subdir = strstr(buildtag.c_str(), "-source") ? "source" : strstr(software.c_str(), "-gui") ? "" : "cli";
  const std::string url = tools::get_update_url(software, subdir, buildtag, version, false);
  const std::string filename = boost::filesystem::path(url).filename().string();
  download_path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("%%%%-%%%%-%%%%-%%%%-" + filename);
  download_done = false;
  download_success = false;

  add_message("Downloading " + url + " to " + download_path.string());

  auto on_result = [this](const std::string &path, const std::string &url, bool success)
  {
    add_message(std::string("Download finished: ") + (success ? "success" : "failed"));
    download_done = true;
    download_success = success;
    emit downloadFinished(success);
    download_handle = NULL;
  };

  auto on_progress = [this](const std::string &path, const std::string &uri, size_t length, ssize_t content_length)
  {
    emit downloadProgress(length, content_length);
    return true;
  };

  download_handle = tools::download_async(download_path.string(), url, on_result, on_progress);
  emit downloadStarted();
}

void Updater::retryDownload()
{
  if (state == StateDownloadFailed)
    set_state(StateDownload);
}

void Updater::select(const QString &qs)
{
  std::string s = qs.toStdString();
  if (s == "gui")
  {
    software = "monero-gui";
    set_state(StateQueryDNS);
    return;
  }
  if (s == "cli")
  {
    software = "monero";
    set_state(StateQueryDNS);
    return;
  }
  MERROR("Invalid selection: " << s);
}

void Updater::check_hash()
{
  std::string path;
  {
    boost::unique_lock<boost::mutex> lock(mutex);
    setHashValid(TriState::TriUnknown);
    path = download_path.string();
  }

  uint8_t file_hash[32];
  bool res = tools::sha256sum(download_path.string(), file_hash);

  boost::unique_lock<boost::mutex> lock(mutex);

  if (!res)
  {
    add_message("Error calculating file hash");
    setHashValid(TriState::TriFalse);
    return;
  }
  std::string file_hash_as_text;
  file_hash_as_text.resize(64);
  static const char digits[] = "0123456789abcdef";
  for (int i = 0; i < 32; ++i)
  {
    file_hash_as_text[i * 2] = digits[file_hash[i] >> 4];
    file_hash_as_text[i * 2 + 1] = digits[file_hash[i] & 0xf];
  }
  if (file_hash_as_text != expected_hash)
  {
    add_message("Invalid file hash");
    setHashValid(TriState::TriFalse);
    return;
  }
  add_message("Update verified, hash " + file_hash_as_text);
  emit validUpdateReady(QString::fromStdString(download_path.string()));
  setHashValid(TriState::TriTrue);
}

#ifdef _WIN32
static std::string find_gpg_directory()
{
  const char *path = getenv("PATH");
  if (!path)
  {
    MDEBUG("Empty PATH");
    return "";
  }

  MDEBUG("PATH: " << path);
  QStringList filter("gpg.exe");

  std::vector<std::string> directories;
  boost::split(directories, path, boost::is_any_of(";"));
  for (const std::string &directory: directories)
  {
    MDEBUG("Looking in " << directory);
    QDir d(QString::fromStdString(directory));
    QFileInfoList list = d.entryInfoList(filter);
    if (!list.empty())
    {
      MINFO("gpg binary found in " << directory);
      return directory;
    }
  }
  MINFO("gpg binary not found");
  return "";
}
#endif

bool Updater::init_gpgme()
{
  gpg_home = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("%%%%-%%%%-%%%%-%%%%");
  set_strict_default_file_permissions(true);
  boost::filesystem::create_directories(gpg_home);
  set_strict_default_file_permissions(false);
  static char env[256];
  snprintf(env, sizeof(env), "GNUPGHOME=%s", gpg_home.string().c_str());
  putenv(env);

  gpg_error_t err;
#ifdef _WIN32
  std::string gpgdir = find_gpg_directory();
  if (!gpgdir.empty())
    gpgme_set_global_flag("w32-inst-dir", gpgdir.c_str());
  gpgme_set_global_flag("disable-gpgconf", "1");
  gpgme_set_global_flag("gpg-name", "gpg");
#endif
  gpgme_check_version(NULL);
  err = gpgme_engine_check_version(GPGME_PROTOCOL_OpenPGP);
  if (err)
  {
    printf("Failed to initialize gpgme: %s\n", gpg_strerror(err));
    return false;
  }

  err = gpgme_new(&ctx);
  if (err)
  {
    printf("Failed to create context: %s\n", gpg_strerror(err));
    return false;
  }

  return true;
}

TriState::tristate_t Updater::verify_gitian_signature(const std::string &contents, const std::string &signature, std::string &fingerprint)
{
  gpgme_data_t contents_data, signature_data;
  gpg_error_t err;

  err = gpgme_data_new_from_mem(&contents_data, contents.data(), contents.size(), 0);
  if (err)
  {
    printf("Failed to create contents data: %s\n", gpg_strerror(err));
    return TriState::TriUnknown;
  }
  err = gpgme_data_new_from_mem(&signature_data, signature.data(), signature.size(), 0);
  if (err)
  {
    printf("Failed to create signature data: %s\n", gpg_strerror(err));
    return TriState::TriUnknown;
  }

  err = gpgme_op_verify(ctx, signature_data, contents_data, NULL);
  gpgme_data_release(signature_data);
  gpgme_data_release(contents_data);
  if (err)
  {
    printf("Failed to verify signature: %s\n", gpg_strerror(err));
    return TriState::TriFalse;
  }
  gpgme_verify_result_t result = gpgme_op_verify_result(ctx);
  if (!result || !result->signatures)
  {
    printf("Failed to get signature verification results\n");
    return TriState::TriFalse;
  }
  fingerprint = result->signatures->fpr;
  if (result->signatures->status)
  {
    printf("Cannot check signature\n");
    return TriState::TriUnknown;
  }
  if (result->signatures->summary & GPGME_SIGSUM_RED)
  {
    printf("Red signature\n");
    return TriState::TriFalse;
  }
  if (result->signatures->summary & GPGME_SIGSUM_VALID)
  {
    printf("Valid signature\n");
    return TriState::TriTrue;
  }

#if 0
  printf("SIG:\n");
  printf("valid: %d\n", result->signatures->summary & GPGME_SIGSUM_VALID);
  printf("green: %d\n", result->signatures->summary & GPGME_SIGSUM_GREEN);
  printf("red: %d\n", result->signatures->summary & GPGME_SIGSUM_RED);
  printf("sys-error: %d\n", result->signatures->summary & GPGME_SIGSUM_SYS_ERROR);
  printf("tofu: %d\n", result->signatures->summary & GPGME_SIGSUM_TOFU_CONFLICT);
  printf("full: %x\n", result->signatures->summary);
  printf("status: %x (%s)\n", result->signatures->status, gpgme_strerror(result->signatures->status));
  printf("validity: %x\n", result->signatures->validity);
  printf("validity_reason: %x (%s)\n", result->signatures->validity_reason, gpgme_strerror(result->signatures->validity_reason));
#endif

  return TriState::TriTrue;
}

void Updater::import_pubkeys()
{
  boost::unique_lock<boost::mutex> lock(mutex);
  gpg_error_t err;

  gitian_pubkeys_import_done = false;
  gitian_pubkeys_import_success = false;

  if (!init_gpgme())
  {
    add_message("Failed to initialize GPG");
    gitian_pubkeys_import_done = true;
    gitian_pubkeys_import_success = false;
    return;
  }
  lock.unlock();

  for (const auto &e: pubkeys)
  {
    gpgme_data_t pubkey_data;

    err = gpgme_data_new_from_mem(&pubkey_data, e.second.data(), e.second.size(), 0);
    if (err)
    {
      printf("Failed to create pubkey data: %s\n", gpg_strerror(err));
      lock.lock();
      gitian_pubkeys_import_done = true;
      gitian_pubkeys_import_success = false;
      return;
    }
    err = gpgme_op_import(ctx, pubkey_data);
    if (err)
    {
      printf("Failed to import pubkey: %s\n", gpg_strerror(err));
      lock.lock();
      gitian_pubkeys_import_done = true;
      gitian_pubkeys_import_success = false;
      return;
    }
    const gpgme_import_result_t result = gpgme_op_import_result(ctx);
    if (!result || !result->imports || !result->imports->fpr || result->imports->result)
    {
      printf("Failed to get results of pubkey import\n");
      lock.lock();
      gitian_pubkeys_import_done = true;
      gitian_pubkeys_import_success = false;
      return;
    }
    const std::string fingerprint = result->imports->fpr;
    gpgme_key_t key;
    err = gpgme_get_key(ctx, fingerprint.c_str(), &key, 0);
    if (err)
    {
      printf("Failed to get imported pubkey");
      lock.lock();
      gitian_pubkeys_import_done = true;
      gitian_pubkeys_import_success = false;
      return;
    }
    err = gpgme_op_tofu_policy(ctx, key, GPGME_TOFU_POLICY_GOOD);
    if (err)
    {
      printf("Failed to set trust policy: %s\n", gpg_strerror(err));
      lock.lock();
      gitian_pubkeys_import_done = true;
      gitian_pubkeys_import_success = false;
      return;
    }

    lock.lock();
    add_message("Imported key " + fingerprint + " from " + e.first);
    imported_fingerprints[fingerprint] = e.first;
    lock.unlock();
    gpgme_key_release(key);
  }

  lock.lock();
  gitian_pubkeys_import_done = true;
  gitian_pubkeys_import_success = true;
}

void Updater::fetch_gitian_sigs()
{
  boost::unique_lock<boost::mutex> lock(mutex);

  gitian_verify_sigs_success = false;
  gitian_verify_sigs_success = false;
  bad_gitian_signature_found = false;

  setTotalGitianSigs(0);
  setProcessedGitianSigs(0);

  boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path("%%%%-%%%%-%%%%-%%%%");
  std::string platform = buildtag;
  auto idx = platform.find('-');
  if (idx != std::string::npos)
    platform = platform.substr(0, idx);
  auto it = platform_to_gitian.find(platform);
  if (it != platform_to_gitian.end())
    platform = it->second;
  std::string base_tree_url_path = "/monero-project/gitian.sigs/tree/master/v" + version + "-" + platform;
  std::string base_blob_url_path = "/monero-project/gitian.sigs/master/v" + version + "-" + platform;
  std::string base_tree_url = "https://github.com" + base_tree_url_path;
  std::string base_blob_url = "https://raw.githubusercontent.com" + base_blob_url_path;
  add_message("Fetching Gitian signatures from " + base_tree_url);
  lock.unlock();
  if (!tools::download(path.string(), base_tree_url))
  {
    lock.lock();
    add_message("Gitian signatures not found");
    setValidGitianSigs(0);
    gitian_verify_sigs_done = true;
    gitian_verify_sigs_success = false;
    lock.unlock();
    set_state(StateNoGitianSigs);
    return;
  }
  std::string s;
  if (!epee::file_io_utils::load_file_to_string(path.string(), s))
  {
    lock.lock();
    add_message("Gitian signatures not found");
    setValidGitianSigs(0);
    gitian_verify_sigs_done = true;
    gitian_verify_sigs_success = false;
    lock.unlock();
    set_state(StateNoGitianSigs);
    return;
  }
  boost::system::error_code ec;
  boost::filesystem::remove(path.string(), ec);

  const std::string subdir = strstr(buildtag.c_str(), "-source") ? "source" : strstr(software.c_str(), "-gui") ? "" : "cli";
  it = dnssec_to_gitian.find(buildtag);
  const std::string gitian_tag = it == dnssec_to_gitian.end() ? buildtag : it->second;
  const std::string url = tools::get_update_url(software, subdir, gitian_tag, version, false);
  std::string filename = boost::filesystem::path(url).filename().string();

  std::string expression = "([abcdefABCDEF0123456789]+)  " + filename + "$";
  STATIC_REGEXP_EXPR_1(rexp_match_hash_and_filename, expression, boost::regex::normal);

  setValidGitianSigs(0);
  setMinValidGitianSigs(MIN_GITIAN_SIGS);
  std::vector<std::string> users;
  idx = 0;
  std::string link_prefix = "href=\"" + base_tree_url_path;
  while (1)
  {
    idx = s.find(link_prefix, idx);
    if (idx == std::string::npos)
      break;
    auto idx2 = s.find("\"", idx + link_prefix.size());
    if (idx2 == std::string::npos || idx2 + 2 >= s.size())
      break;
    std::string user = s.substr(idx + link_prefix.size() + 1 , idx2 - idx - link_prefix.size() - 1);
    idx = idx2;
    if (user.size() > 20 || strspn(user.c_str(), "abcdefghijlkmnopqrstuvwxyzABCDEFGHIJLKMNOPQRSTUVWXYZ_-0123456789") != user.size())
      continue;
    users.push_back(std::move(user));
  }

  if (users.empty())
  {
    lock.lock();
    gitian_verify_sigs_done = true;
    gitian_verify_sigs_success = false;
    add_message("No Gitian signatures found");
    lock.unlock();
    set_state(StateNoGitianSigs);
    return;
  }

  set_state(StateVerifyGitianSignatures);
  setTotalGitianSigs(users.size());
  std::map<std::string, std::string> fingerprints;

  for (const std::string &user:users)
  {
    std::string short_version = version.substr(0, 4);
    std::string assert_url = base_blob_url + "/" + user + "/" + software + "-" + platform + "-" + short_version + "-build.assert";
    std::string sig_url = base_blob_url + "/" + user + "/" + software + "-" + platform + "-" + short_version + "-build.assert.sig";
    std::string assert_contents, sig_contents;
    boost::filesystem::remove(path.string(), ec);
    if (tools::download(path.string(), assert_url) && epee::file_io_utils::load_file_to_string(path.string(), assert_contents))
    {
      boost::filesystem::remove(path.string(), ec);
      if (tools::download(path.string(), sig_url) && epee::file_io_utils::load_file_to_string(path.string(), sig_contents))
      {
        std::string fingerprint;
        tristate_t res = verify_gitian_signature(assert_contents, sig_contents, fingerprint);
        const auto it = fingerprints.find(fingerprint);
        if (res == TriState::TriTrue && it == fingerprints.end() && imported_fingerprints.find(fingerprint) != imported_fingerprints.end())
        {
          bool found = false;
          std::string hash;
          std::vector<std::string> lines;
          boost::split(lines, assert_contents, boost::is_any_of("\n"));
          for (const auto &line: lines)
          {
            boost::smatch result;
            if (boost::regex_search(line, result, rexp_match_hash_and_filename, boost::match_default) && result[0].matched)
            {
              hash = result[1];
              found = true;
            }
          }
          if (!found)
          {
            lock.lock();
            add_message("No hash found in Gitian assert file for " + filename + " from " + user);
            lock.unlock();
          }
          else if (hash != expected_hash)
          {
            lock.lock();
            add_message("Gitian hash does not match expected hash for " + filename + " from " + user);
            lock.unlock();
          }
          else
          {
            lock.lock();
            add_message("Good Gitian signature with matching hash from " + user + ", fingerprint " + fingerprint);
            setValidGitianSigs(validGitianSigs + 1);
            lock.unlock();
            fingerprints.insert(std::make_pair(fingerprint, user));
          }
        }
        else if (res == TriState::TriTrue && it == fingerprints.end() && imported_fingerprints.find(fingerprint) == imported_fingerprints.end())
        {
          lock.lock();
          add_message("Valid Gitian signature from " + user + ", but from key " + fingerprint + " which is not the one on record");
          lock.unlock();
        }
        else if (res == TriState::TriTrue && it != fingerprints.end())
        {
          lock.lock();
          add_message("Duplicate Gitian signature from " + user + ", previously seen from " + it->second + ", fingerprint " + fingerprint);
          lock.unlock();
        }
        else if (res == TriState::TriFalse)
        {
          lock.lock();
          add_message("Bad Gitian signature from " + user);
          lock.unlock();
          bad_gitian_signature_found = true;
        }
        else
        {
          lock.lock();
          add_message("Inconclusive Gitian signature from " + user + ", fingerprint " + fingerprint);
          lock.unlock();
        }
      }
      else
        add_message("Failed to fetch " + sig_url);
    }
    else
      add_message("Failed to fetch " + assert_url);
    setProcessedGitianSigs(processedGitianSigs + 1);
  }
  boost::filesystem::remove(path.string(), ec);
  boost::filesystem::remove_all(gpg_home.string(), ec);
  lock.lock();
  gitian_verify_sigs_done = true;
  gitian_verify_sigs_success = validGitianSigs >= MIN_GITIAN_SIGS && !bad_gitian_signature_found;
}

void Updater::add_message(const std::string &s)
{
  MINFO("UI message: " << s);
  messages.push_back(s);
  emit message(QString::fromStdString(s));
}

void Updater::updater_thread()
{
  while (1)
  {
    {
      boost::unique_lock<boost::mutex> lock(mutex);
      if (!running)
        break;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    change_state();

    switch (state)
    {
    case StateQueryDNS:
      if (!dns_query_done)
        break;
      if (good_dns_records.empty())
        set_state(StateDNSFailed);
      else
        set_state(StateCheckVersion);
      break;
    case StateCheckVersion:
      if (!version_check_done)
        break;
      if (version.empty())
        set_state(StateNoUpdateInfoFound);
      else
      {
        int cmp = tools::vercmp(version.c_str(), current_version.c_str());
        if (cmp > 0)
          set_state(StateImportPubkeys);
        else if (cmp < 0)
          set_state(StateBackInTime);
        else
          set_state(StateUpToDate);
      }
      break;
    case StateImportPubkeys:
      if (!gitian_pubkeys_import_done)
        break;
      if (gitian_pubkeys_import_success)
        set_state(StateFetchGitianSigs);
      else
        set_state(StatePubkeyImportFailed);
      break;
    case StateVerifyGitianSignatures:
      if (!gitian_verify_sigs_done)
        break;
      if (gitian_verify_sigs_success)
        set_state(StateDownload);
      else if (!bad_gitian_signature_found)
        set_state(StateNotEnoughGitianSigs);
      else
        set_state(StateBadGitianSigs);
      break;
    case StateDownload:
      if (!download_done)
        break;
      if (download_success)
        set_state(StateCheckHash);
      else
        set_state(StateDownloadFailed);
      break;
    case StateCheckHash:
      if (hashValid == TriState::TriTrue)
        set_state(StateValidUpdate);
      else if (hashValid == TriState::TriFalse)
        set_state(StateBadHash);
    default:
      break;
    }
  }
}

void Updater::set_state(State s)
{
  {
    boost::unique_lock<boost::mutex> lock(mutex);
    next_state = s;
  }
}

void Updater::change_state()
{
  State s = StateNone;
  bool selecting = false;
  {
    boost::unique_lock<boost::mutex> lock(mutex);
    if (state == next_state)
      return;
    state = next_state;
    s = state;
    selecting = getSelecting();
  }

  emit stateChanged(get_state_name(s));
  emit stateOutcomeChanged(get_state_outcome(s));
  emit selectingChanged(selecting);

  switch (s)
  {
    case StateInit:
      {
        boost::unique_lock<boost::mutex> lock(mutex);
        dns_query_done = false;
        version_check_done = false;
        setDnsValid(TriState::TriUnknown);
        setHashValid(TriState::TriUnknown);
        setValidGitianSigs(0);
        setMinValidGitianSigs(0);
        bad_gitian_signature_found = false;
      }
      break;
    case StateQueryDNS:
      load_txt_records_from_dns(dns_urls, dns_query_results, good_dns_records);
      break;
    case StateCheckVersion:
      process_version(software, buildtag, good_dns_records);
      break;
    case StateDownload:
      start_download();
      break;
    case StateCheckHash:
      check_hash();
      break;
    case StateImportPubkeys:
      import_pubkeys();
      break;
    case StateFetchGitianSigs:
      fetch_gitian_sigs();
      break;
    default:
      break;
  }
}

QString Updater::getState() const
{
  boost::unique_lock<boost::mutex> lock(mutex);
  return get_state_name(state);
}

TriState::tristate_t Updater::getStateOutcome() const
{
  boost::unique_lock<boost::mutex> lock(mutex);
  return get_state_outcome(state);
}

QString Updater::getVersion() const
{
  boost::unique_lock<boost::mutex> lock(mutex);
  return QString::fromStdString(version);
}

Updater::tristate_t Updater::getDnsValid() const
{
  boost::unique_lock<boost::mutex> lock(mutex);
  return dnsValid;
}

Updater::tristate_t Updater::getHashValid() const
{
  boost::unique_lock<boost::mutex> lock(mutex);
  return hashValid;
}

uint32_t Updater::getValidGitianSigs() const
{
  boost::unique_lock<boost::mutex> lock(mutex);
  return validGitianSigs;
}

uint32_t Updater::getMinValidGitianSigs() const
{
  return minValidGitianSigs;
}

uint32_t Updater::getProcessedGitianSigs() const
{
  return processedGitianSigs;
}

uint32_t Updater::getTotalGitianSigs() const
{
  return totalGitianSigs;
}

bool Updater::getSelecting() const
{
  return state == StateInit;
}
