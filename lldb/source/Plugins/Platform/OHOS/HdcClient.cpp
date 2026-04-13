//===-- HdcClient.cpp -----------------------------------------------------===//
//
// Copyright (c) Huawei Technologies Co., Ltd. 2025. All rights reserved.
// This source file is part of the Cangjie project, licensed under Apache-2.0
// with Runtime Library Exception.
//
// See https://cangjie-lang.cn/pages/LICENSE for license information.
//
//===----------------------------------------------------------------------===//

#include "HdcClient.h"

#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/FileUtilities.h"
#include "llvm/Support/FileSystem.h"

#include "lldb/Host/ConnectionFileDescriptor.h"
#include "lldb/Host/FileSystem.h"
#include "lldb/Host/PosixApi.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/Utility/Timeout.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/DataEncoder.h"
#include "lldb/Utility/DataExtractor.h"

#if defined(_WIN32)
#include <winsock.h>
#else
#include <arpa/inet.h>
#endif

#include <limits>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <utility>
#include <securec.h>
#include <regex>

// On Windows, transitive dependencies pull in <Windows.h>, which defines a
// macro that clashes with a method name.
#ifdef SendMessage
#undef SendMessage
#endif

using namespace lldb;
using namespace lldb_private;
using namespace lldb_private::platform_ohos;
using namespace std::chrono;

namespace {

const seconds kReadTimeout(20);
const char *kSocketNamespaceAbstract = "localabstract";
const char *kSocketNamespaceFileSystem = "localfilesystem";

#define MAX_PACKET_SIZE   67108864U  // 64MB

Status ReadAllBytes(Connection &conn, void *buffer, size_t size) {
  Status error;
  ConnectionStatus status;
  char *read_buffer = static_cast<char *>(buffer);

  auto now = steady_clock::now();
  const auto deadline = now + kReadTimeout;
  size_t total_read_bytes = 0;
  while (total_read_bytes < size && now < deadline) {
    auto read_bytes = conn.Read(read_buffer + total_read_bytes,
                                size - total_read_bytes,
                                duration_cast<microseconds>(deadline - now),
                                status, &error);
    if (error.Fail()) {
      return error;
    }
    total_read_bytes += read_bytes;
    if (status != eConnectionStatusSuccess) {
        break;
    }
    now = steady_clock::now();
  }
  if (total_read_bytes < size) {
    error = Status("Unable to read requested number of bytes. "
                   "Connection status: %d.", status);
  }
  return error;
}
}

Status HdcClient::CreateByDeviceID(const std::string &device_id,
                                   HdcClient &hdc) {
  DeviceIDList connect_devices;
  auto error = hdc.GetDevices(connect_devices);
  if (error.Fail()) {
    return error;
  }

  std::string hdc_utid;
  if (!device_id.empty()) {
    hdc_utid = device_id;
  }

  if (hdc_utid.empty()) {
    if (connect_devices.size() != 1) {
      return Status("Expected a single connected device, got instead %zu - try "
                    "setting 'HDC_UTID'", connect_devices.size());
    }
    hdc.SetDeviceID(connect_devices.front());
  } else {
    auto find_it = std::find(connect_devices.begin(), connect_devices.end(),
                             hdc_utid);
    if (find_it == connect_devices.end()) {
      return Status("Device \"%s\" not found, check HDC_UTID environment variable",
                    hdc_utid.c_str());
    }
    hdc.SetDeviceID(*find_it);
  }
  return error;
}

HdcClient::HdcClient(const std::string &connect_addr,
                     const std::string &device_id)
    : m_device_id(device_id), m_connect_addr(connect_addr) {}

HdcClient::HdcClient(HdcClient &&) = default;

HdcClient::~HdcClient() = default;

void HdcClient::SetDeviceID(const std::string &device_id) {
  m_device_id = device_id;
}

const std::string &HdcClient::GetDeviceID() const { return m_device_id; }

bool HdcClient::IsServerLocal() {
  return m_connect_addr == "localhost" || m_connect_addr == "127.0.0.1";
}

Status HdcClient::Connect() {
  struct ChannelHandShake {
    unsigned size;
    char banner[12];
    char connectKey[32];
  } __attribute__((packed));

  Status error;
  ChannelHandShake handshake = {};
  if (m_device_id.size() > sizeof(handshake.connectKey)) {
    return Status("Device id is too long: %s", m_device_id.c_str());
  }
  m_conn.reset(new ConnectionFileDescriptor);
  std::string port = "8710";
  std::string uri = "connect://" + m_connect_addr + ":" + port;
  m_conn->Connect(uri.c_str(), &error);
  ConnectionStatus status = eConnectionStatusError;
  if (error.Success()) {
    error = ReadAllBytes(&handshake, sizeof(handshake));
    if (error.Success()) {
      memset_s(handshake.connectKey, sizeof(handshake.connectKey), 0, sizeof(handshake.connectKey));
      memcpy_s(handshake.connectKey, sizeof(handshake.connectKey), m_device_id.c_str(), m_device_id.size());
      m_conn->Write(&handshake, sizeof(handshake), status, &error);
    }
  }
  return error;
}

Status HdcClient::GetDevices(DeviceIDList &device_list) {
  device_list.clear();

  auto error = SendMessage("list targets");
  if (error.Fail()) {
    return error;
  }

  std::vector<char> in_buffer;
  error = ReadMessage(in_buffer);

  llvm::StringRef response(&in_buffer[0], in_buffer.size());
  const unsigned vector_size = 4;
  llvm::SmallVector<llvm::StringRef, vector_size> devices;
  response.split(devices, "\n", -1, false);

  for (const auto device : devices) {
    device_list.push_back(static_cast<std::string>(device.split('\t').first));
  }
  // Force disconnect since HDC closes connection after host:devices response is sent.
  m_conn.reset();
  return error;
}

Status HdcClient::SetPortForwarding(const uint16_t local_port,
                                    const uint16_t remote_port) {
  std::stringstream cmd;
  cmd << "fport tcp:" << local_port << " tcp:" << remote_port;

  const auto error = SendMessage(cmd.str());
  if (error.Fail()) {
    return error;
  }
  return ReadResponseStatus("Forwardport result:OK");
}

bool ValidateSocketName(const std::string& name) {
    if (name.empty()) {
        return false;
    }

    const std::regex valid_chars(R"(^[a-zA-Z0-9_.-]+$)");

    return std::regex_match(name, valid_chars);
}

Status HdcClient::SetPortForwarding(const uint16_t local_port,
                                    llvm::StringRef remote_socket_name,
                                    const UnixSocketNamespace socket_namespace) {
  if (!ValidateSocketName(remote_socket_name.str())) {
    return Status("Remote socket name is invalid: %s", remote_socket_name.str().c_str());
  }
  const char *sock_namespace_str = (socket_namespace == UnixSocketNamespace::UnixSocketNamespaceAbstract)
                                   ? kSocketNamespaceAbstract
                                   : kSocketNamespaceFileSystem;
  std::stringstream cmd;
  cmd << "fport tcp:" << local_port << " " << sock_namespace_str
      << ":" << remote_socket_name.str().c_str();
  const auto error = SendMessage(cmd.str());
  if (error.Fail()) {
    return error;
  }
  return ReadResponseStatus("Forwardport result:OK");
}

Status HdcClient::DeletePortForwarding(std::pair<uint16_t, uint16_t> fwd) {
  std::stringstream cmd;
  cmd << "fport rm tcp:" << fwd.first << " tcp:" << fwd.second;

  const auto error = SendMessage(cmd.str());
  if (error.Fail()) {
    return error;
  }
  return ReadResponseStatus("Remove forward ruler success");
}

Status HdcClient::DeletePortForwarding(const uint16_t local_port,
                                       const std::string remote_socket_name,
                                       const UnixSocketNamespace socket_namespace) {
  const char *sock_namespace_str = (socket_namespace == UnixSocketNamespace::UnixSocketNamespaceAbstract)
                                    ? kSocketNamespaceAbstract
                                    : kSocketNamespaceFileSystem;
  std::stringstream cmd;
  cmd << "fport rm tcp:" << local_port << " " << sock_namespace_str
      << ":" << remote_socket_name.c_str();
  const auto error = SendMessage(cmd.str());
  if (error.Fail()) {
    return error;
  }
  return ReadResponseStatus("Remove forward ruler success");
}

static std::string EscapeHdcPath(const std::string& path) {
  std::string result = "\"";
  for (char c : path) {
    if (c == '"' || c == '\\') {
      result += '\\';
    }
    result += c;
  }
  result += "\"";
  return result;
}

Status HdcClient::LocalTransferFile(const char *direction, const FileSpec &src,
                                    const FileSpec &dst) {
  const unsigned vector_size = 128;
  llvm::SmallVector<char, vector_size> cwd;
  std::error_code ec = llvm::sys::fs::current_path(cwd);
  if (ec)
    return Status(ec);

  std::stringstream cmd;
  cmd << "file " << direction << " -cwd ";
  cmd.write(cwd.data(), cwd.size());
  cmd << " " << EscapeHdcPath(src.GetPath()) << " " << EscapeHdcPath(dst.GetPath());
  Status error = SendMessage(cmd.str());
  if (error.Fail())
    return error;

  return ReadResponseStatus("FileTransfer finish");
}

Status HdcClient::ExpectCommandMessagePrefix(uint16_t expected_command,
                                             std::vector<char> &message,
                                             size_t prefix_size) {
  uint16_t command;
  Status error = ReadCommandMessagePrefix(command, message, prefix_size);
  if (error.Fail())
    return error;
  if (command != expected_command)
    return Status("Unexpected HDC server command: %d, expected %d", command,
                  expected_command);

  return error;
}

Status HdcClient::ExpectCommandMessage(uint16_t expected_command,
                                       std::vector<char> &message) {
  return ExpectCommandMessagePrefix(expected_command, message,
                                    std::numeric_limits<size_t>::max());
}

template <typename T> struct HdcIO;

struct HdcTagIO {
  enum class WireType : uint32_t { VARINT = 0, LENGTH_DELIMETED = 2 };

  template <typename InItT, typename U = uint32_t>
  static llvm::Optional<std::pair<size_t, WireType>> ParseTag(InItT &InBegin,
                                                              InItT InEnd) {
    llvm::Optional<U> Tag = HdcIO<U>::Parse(InBegin, InEnd);
    return Tag.map([](U Val) {
      // Move by 3 bits to obtain the position of the write type.
      return std::make_pair(static_cast<size_t>(Val >> 3),
                            static_cast<WireType>(Val & 0x7));
    });
  }

  template <typename OutItT, typename U = uint32_t>
  static void SerializeTag(size_t Idx, WireType Type, OutItT &OutIt) {
    // Move by 3 bits to obtain the position of the write type.
    HdcIO<U>::Serialize(static_cast<uint32_t>(Type) | (static_cast<uint32_t>(Idx) << 3), OutIt);
  }
};

template <HdcTagIO::WireType TheType, typename T, class DerivedT>
struct HdcIOBase {
  using ValueT = T;

  static HdcTagIO::WireType GetWireType() { return TheType; }

  template <typename InItT>
  static llvm::Optional<T> ParseTagged(size_t Idx, InItT &InBegin,
                                       InItT InEnd) {
    if (!HdcTagIO::ParseTag(InBegin, InEnd)
             .map([Idx](auto P) {
               return P.first == Idx && P.second == TheType;
             })
             .value_or(false))
      return {};
    return DerivedT::Parse(InBegin, InEnd);
  }

  template <typename OutItT>
  static void SerializeTagged(size_t Idx, T Value, OutItT &OutIt) {
    HdcTagIO::SerializeTag(Idx, TheType, OutIt);
    DerivedT::Serialize(Value, OutIt);
  }
};

template <typename T>
struct HdcIO : HdcIOBase<HdcTagIO::WireType::VARINT, T, HdcIO<T>> {
  static_assert(std::is_integral<T>::value, "Don't know how to parse T");

  template <typename InItT>
  static llvm::Optional<T> Parse(InItT &InBegin, InItT InEnd) {
    constexpr size_t NBytes = (sizeof(T) * 8 + 6) / 7;
    T value = 0;
    for (size_t c = 0; c < NBytes; ++c) {
      uint8_t x;
      if (InBegin == InEnd)
        return {};
      x = *InBegin++;
      // Move by 7 bits to obtain the position of tag
      value |= static_cast<T>(x & 0x7Fu) << 7 * c;
      if (!(x & 0x80))
        return value;
    }
    return {};
  }

  template <typename OutItT> static void Serialize(T value, OutItT &OutIt) {
    constexpr size_t NBytes = (sizeof(T) * 8 + 6) / 7;
    uint8_t b[NBytes];
    for (size_t i = 0; i < NBytes; ++i) {
      b[i] = value & 0b0111'1111;
      // Move by 7 bits to obtain the position of tag
      value >>= 7;
      if (value) {
        b[i] |= 0b1000'0000;
      } else {
        OutIt = std::copy_n(std::begin(b), i + 1, OutIt);
        return;
      }
    }
  }
};

template <>
struct HdcIO<std::string> : HdcIOBase<HdcTagIO::WireType::LENGTH_DELIMETED,
                                      std::string, HdcIO<std::string>> {
  template <typename InItT>
  static llvm::Optional<std::string> Parse(InItT &InBegin, InItT InEnd) {
    auto MaybeLen = HdcIO<size_t>::Parse(InBegin, InEnd);
    if (!MaybeLen)
      return llvm::None;
    size_t Len = *MaybeLen;
    std::string Res;
    Res.reserve(Len);
    while (Res.size() < Len && InBegin != InEnd)
      Res.push_back(*InBegin++);
    if (Res.size() < Len)
      return {};
    return Res;
  }

  template <typename OutItT>
  static void Serialize(const std::string &value, OutItT &OutIt) {
    HdcIO<size_t>::Serialize(value.size(), OutIt);
    OutIt = std::copy_n(value.begin(), value.size(), OutIt);
  }
};

template <size_t StartIdx, typename... Ts> struct HdcTaggedIOHelper {
  template <typename InItT>
  static llvm::Optional<std::tuple<>> Parse(InItT &InBegin, InItT InEnd) {
    return std::tuple<>{};
  }

  template <typename OutItT> static void Serialize(OutItT &OutIt) {}
};

template <size_t StartIdx, typename T, typename... Ts>
struct HdcTaggedIOHelper<StartIdx, T, Ts...> {
  using ResT = llvm::Optional<std::tuple<T, Ts...>>;

  template <typename InItT> static ResT Parse(InItT &InBegin, InItT InEnd) {
    return HdcIO<T>::ParseTagged(StartIdx, InBegin, InEnd)
        .map([&](auto &&LHS) {
          return HdcTaggedIOHelper<StartIdx + 1, Ts...>::Parse(InBegin, InEnd)
              .map([LHS = std::move(LHS)](auto &&RHS) {
                return std::tuple_cat(std::make_tuple(std::move(LHS)),
                                      std::move(RHS));
              });
        })
        .value_or(ResT(llvm::None));
  }

  template <typename InItT> static ResT ParseOnce(InItT InBegin, InItT InEnd) {
    return Parse(InBegin, InEnd);
  }

  template <typename OutItT>
  static void Serialize(const T &Arg, const Ts &...Args, OutItT &OutIt) {
    HdcIO<T>::SerializeTagged(StartIdx, std::move(Arg), OutIt);
    HdcTaggedIOHelper<StartIdx + 1, Ts...>::Serialize(std::move(Args)...,
                                                      OutIt);
  }

  template <typename OutItT>
  static void SerializeOnce(const T &Arg, const Ts &...Args, OutItT OutIt) {
    return Serialize(Arg, Args..., OutIt);
  }
};

template <typename... Ts> struct HdcTaggedIO : HdcTaggedIOHelper<1, Ts...> {};

template <typename TupleT> struct HdcTaggedIOTuple;

template <typename... Ts>
struct HdcTaggedIOTuple<std::tuple<Ts...>> : HdcTaggedIO<Ts...> {};

using HdcTransferConfig = std::tuple<uint64_t,    // fileSize
                                     uint64_t,    // atime
                                     uint64_t,    // mtime
                                     std::string, // options
                                     std::string, // path
                                     std::string, // optionalName
                                     bool,        // updateIfNew
                                     uint8_t,     // compressType
                                     bool,        // holdTimestamp
                                     std::string, // funcName
                                     std::string, // clientCwd
                                     std::string, // reserve1
                                     std::string  // reserve2
                                     >;

using HdcTransferPayload = std::tuple<uint64_t, // index
                                      uint8_t,  // compressType
                                      uint32_t, // compressSize
                                      uint32_t  // uncompressSize
                                      >;

using HdcFileMode = std::tuple<uint64_t,     // perm
                               uint64_t,     // u_id
                               uint64_t,     // g_id
                               std::string,  // context
                               std::string>; // fullName

enum HdcCommand : uint16_t {
  CMD_KERNEL_WAKEUP_SLAVETASK = 12,
  CMD_FILE_INIT = 3000,
  CMD_FILE_CHECK,
  CMD_FILE_BEGIN,
  CMD_FILE_DATA,
  CMD_FILE_FINISH,
  CMD_FILE_MODE = 3006
};

static constexpr size_t HdcTransferPayloadPrefixReserve = 64;
static constexpr size_t HdcTransferPayloadMaxChunkSize = 49152;

Status HdcClient::FileCheck(int FD, size_t &file_size) {
  std::vector<char> msg;
  Status error = ExpectCommandMessage(HdcCommand::CMD_FILE_MODE, msg);
  if (error.Fail())
    return error;

  auto maybe_file_mode = HdcTaggedIOTuple<HdcFileMode>::ParseOnce(msg.cbegin(), msg.cend());
  if (!maybe_file_mode)
    return Status("Could not parse HDC server FileMode");

  auto &file_mode = *maybe_file_mode;

  uint32_t perms = static_cast<uint32_t>(std::get<0>(file_mode));
  auto EC = llvm::sys::fs::setPermissions(
      FD, static_cast<llvm::sys::fs::perms>(perms) & llvm::sys::fs::all_perms);
  if (EC)
    return Status(EC);

  error = SendCommandMessage(HdcCommand::CMD_FILE_MODE, {});
  if (error.Fail())
    return error;

  error = ExpectCommandMessage(HdcCommand::CMD_FILE_CHECK, msg);
  if (error.Fail())
    return error;

  auto transfer_config =
      HdcTaggedIOTuple<HdcTransferConfig>::ParseOnce(msg.cbegin(), msg.cend());
  if (!transfer_config.has_value())
    return Status("Could not parse HDC server TransferConfig");

  if (auto compress_type = std::get<7>(*transfer_config))
    return Status("Compression is not supported");

  file_size = std::get<0>(*transfer_config);

  return SendCommandMessage(HdcCommand::CMD_FILE_BEGIN, {});
}

Status HdcClient::PullFileChunk(std::vector<char> &buffer) {
  buffer.clear();

  std::vector<char> msg;
  Status error = ExpectCommandMessagePrefix(HdcCommand::CMD_FILE_DATA, msg,
                                            HdcTransferPayloadPrefixReserve);
  if (error.Fail())
    return error;

  auto transfer_payload =
      HdcTaggedIOTuple<HdcTransferPayload>::ParseOnce(msg.cbegin(), msg.cend());
  if (!transfer_payload.has_value())
    return Status("Could not parse HDC server TransferPayload");

  if (auto compress_type = std::get<1>(*transfer_payload))
    return Status("Compression is not supported");

  uint32_t read_bytes = std::get<3>(*transfer_payload);
  if (read_bytes >= MAX_PACKET_SIZE) {
    return Status("Read transfer payload too large: %u", read_bytes);
  }
  buffer.resize(read_bytes, 0);
  error = ReadAllBytes(buffer.data(), buffer.size());
  if (error.Fail())
    buffer.clear();

  return error;
}

Status HdcClient::RecvFile(const FileSpec &src, const FileSpec &dst) {
  if (IsServerLocal())
    return LocalTransferFile("recv", src, dst);

  const auto local_file_path = dst.GetPath();
  llvm::FileRemover local_file_remover(local_file_path);

  int dst_file_fd;
  auto EC = llvm::sys::fs::openFileForWrite(local_file_path, dst_file_fd);
  if (EC)
    return Status("Unable to open local file %s", local_file_path.c_str());

  std::stringstream cmd;
  cmd << "file recv remote -m";
  cmd << " " << EscapeHdcPath(src.GetPath()) << " " << EscapeHdcPath(dst.GetPath());
  Status error = SendMessage(cmd.str());
  if (error.Fail())
    return error;

  size_t cur_size = 0;
  size_t all_size = 0;
  error = FileCheck(dst_file_fd, all_size);
  if (error.Fail())
    return error;

  llvm::raw_fd_ostream dst_file(dst_file_fd, true);

  std::vector<char> buf;
  while (cur_size < all_size) {
    error = PullFileChunk(buf);
    if (error.Fail()) {
      dst_file.close();
      return error;
    }
    if (buf.empty()) {
      break;
    }
    dst_file.write(buf.data(), buf.size());
    cur_size += buf.size();
  }

  error = SendCommandMessage(HdcCommand::CMD_FILE_FINISH, {});
  if (error.Fail()) {
    dst_file.close();
    return error;
  }
  error = ReadResponseStatus("FileTransfer finish");
  if (error.Fail()) {
    dst_file.close();
    return error;
  }

  dst_file.close();
  if (dst_file.has_error())
    return Status("Failed to write file %s", local_file_path.c_str());

  local_file_remover.releaseFile();
  return error;
}

Status HdcClient::FileInit(size_t file_size, uint32_t perm, uint32_t u_id,
                           uint32_t g_id, const std::string &remote_path) {
  std::vector<char> msg;
  Status error = ExpectCommandMessage(HdcCommand::CMD_FILE_INIT, msg);
  if (error.Fail())
    return error;

  error = SendCommandMessage(HdcCommand::CMD_KERNEL_WAKEUP_SLAVETASK, {});
  if (error.Fail())
    return error;

  constexpr uint64_t IFREG_MASK = 0100000;
  msg.clear();
  HdcTaggedIOTuple<HdcFileMode>::SerializeOnce(perm | IFREG_MASK, // perm
                                               u_id,              // u_id
                                               g_id,              // g_id
                                               "",                // context
                                               "",                // fullName
                                               std::back_inserter(msg));
  error = SendCommandMessage(HdcCommand::CMD_FILE_MODE, msg);
  if (error.Fail())
    return error;

  error = ExpectCommandMessage(HdcCommand::CMD_FILE_MODE, msg);
  if (error.Fail())
    return error;

  msg.clear();
  HdcTaggedIOTuple<HdcTransferConfig>::SerializeOnce(file_size,   // fileSize
                                                     0,           // atime
                                                     0,           // mtime
                                                     "",          // options
                                                     remote_path, // path
                                                     "",    // optionalName
                                                     false, // updateIfNew
                                                     0,     // compressType
                                                     false, // holdTimestamp
                                                     "",    // funcName
                                                     "",    // clientCwd
                                                     "",    // reserve1
                                                     "",    // reserve2
                                                     std::back_inserter(msg));
  error = SendCommandMessage(HdcCommand::CMD_FILE_CHECK, msg);
  if (error.Fail())
    return error;

  return ExpectCommandMessage(HdcCommand::CMD_FILE_BEGIN, msg);
}

Status HdcClient::PushFileChunk(std::vector<char> &buffer, size_t chunk_size,
                                size_t index) {
  std::fill_n(buffer.begin(), HdcTransferPayloadPrefixReserve, 0);
  HdcTaggedIOTuple<HdcTransferPayload>::SerializeOnce(
      index,      // index
      0,          // compressType
      chunk_size, // compressSize
      chunk_size, // uncompressSize
      buffer.begin());
  return SendCommandMessage(CMD_FILE_DATA, buffer);
}

Status HdcClient::SendFile(const FileSpec &src, const FileSpec &dst) {
  if (IsServerLocal())
    return LocalTransferFile("send", src, dst);

  const auto local_file_path = src.GetPath();
  std::ifstream src_file(local_file_path.c_str(), std::ios::in | std::ios::binary);
  if (!src_file.is_open())
    return Status("Unable to open local file %s", local_file_path.c_str());

  std::stringstream cmd;
  cmd << "file send remote -m " << EscapeHdcPath(src.GetPath()) << " " << EscapeHdcPath(dst.GetPath());
  Status error = SendMessage(cmd.str());
  if (error.Fail())
    return error;

  llvm::sys::fs::file_status status;
  auto EC = llvm::sys::fs::status(local_file_path, status);
  if (EC)
    return Status(EC);

  error = FileInit(status.getSize(), status.permissions(), status.getUser(),
                   status.getGroup(), dst.GetPath());
  if (error.Fail())
    return error;

  std::vector<char> buffer;
  size_t sent_bytes = 0;
  while (!src_file.eof()) {
    buffer.resize(HdcTransferPayloadPrefixReserve +
                  HdcTransferPayloadMaxChunkSize);
    if (src_file
            .read(buffer.data() + HdcTransferPayloadPrefixReserve,
                  HdcTransferPayloadMaxChunkSize)
            .bad())
      break;
    size_t chunk_size = src_file.gcount();
    buffer.resize(HdcTransferPayloadPrefixReserve + chunk_size);
    error = PushFileChunk(buffer, chunk_size, sent_bytes);
    if (error.Fail())
      return error;
    sent_bytes += chunk_size;
  }

  if (src_file.bad())
    return Status("Failed read on %s", local_file_path.c_str());
  if (sent_bytes < status.getSize()) {
    m_conn.reset();
    return Status("Failed to read all of the bytes from %s: read %zu/%zu",
                  local_file_path.c_str(), sent_bytes, status.getSize());
  }

  error = ExpectCommandMessage(HdcCommand::CMD_FILE_FINISH, buffer);
  if (error.Fail())
    return error;

  error = SendCommandMessage(HdcCommand::CMD_FILE_FINISH, {});
  if (error.Fail())
    return error;

  return ReadResponseStatus("FileTransfer finish");
}

Status HdcClient::SendMessage(llvm::StringRef packet, const bool reconnect) {
  Status error;
  if (!m_conn || reconnect) {
    error = Connect();
    if (error.Fail()) {
      return error;
    }
  }
  if (packet.size() >= MAX_PACKET_SIZE) {
    return Status("Send message packet too large: %lu", packet.size());
  }

  unsigned msg_len = packet.size() + 1;
  const unsigned vector_size = 128;
  llvm::SmallVector<char, vector_size> message(msg_len + sizeof(unsigned), 0);
  unsigned len = htonl(msg_len);
  memcpy_s(message.data(), message.size(), &len, sizeof(len));
  memcpy_s(message.data() + sizeof(len), message.size() - sizeof(len), packet.data(), packet.size());

  ConnectionStatus status;
  m_conn->Write(message.data(), message.size(), status, &error);
  if (error.Fail()) {
    return error;
  }

  return error;
}

Status HdcClient::SendCommandMessage(uint16_t command, llvm::ArrayRef<char> packet) {
  llvm::SmallVector<char> buf(sizeof(command) + packet.size());
  std::copy_n(reinterpret_cast<char *>(&command), sizeof(command), buf.begin());
  std::copy_n(packet.begin(), packet.size(), buf.begin() + sizeof(command));
  return SendMessage(llvm::StringRef(buf.data(), buf.size()), false);
}

Status HdcClient::ReadMessage(std::vector<char> &message) {
  message.clear();

  unsigned packet_len;
  auto error = ReadAllBytes(&packet_len, sizeof(packet_len));
  if (error.Fail()) {
    return error;
  }

  packet_len = htonl(packet_len);
  if (packet_len >= MAX_PACKET_SIZE) {
    return Status("Read message packet too large: %u", packet_len);
  }
  message.resize(packet_len, 0);
  error = ReadAllBytes(&message[0], packet_len);
  if (error.Fail()) {
    message.clear();
  }

  return error;
}

Status HdcClient::ReadCommandMessagePrefix(uint16_t &command, std::vector<char> &message, size_t prefix_size) {
  message.clear();

  unsigned packet_len;
  auto error = ReadAllBytes(&packet_len, sizeof(packet_len));
  if (error.Fail())
    return error;

  packet_len = htonl(packet_len);
  if (packet_len < sizeof(command))
    return Status("Message too small to contain a command");

  error = ReadAllBytes(&command, sizeof(command));
  if (error.Fail()) {
    command = 0;
    return error;
  }

  message.resize(std::min(packet_len - sizeof(command), prefix_size), 0);
  error = ReadAllBytes(&message[0], message.size());
  if (error.Fail()) {
    command = 0;
    message.clear();
  }

  return error;
}

Status HdcClient::ReadCommandMessage(uint16_t &command,
                                     std::vector<char> &message) {
  return ReadCommandMessagePrefix(command, message,
                                  std::numeric_limits<size_t>::max());
}

Status HdcClient::ReadMessageStream(std::vector<char> &message,
                                    milliseconds timeout) {
  auto start = steady_clock::now();
  message.clear();

  Status error;
  lldb::ConnectionStatus status = lldb::eConnectionStatusSuccess;
  char buffer[1024];
  while (error.Success() && status == lldb::eConnectionStatusSuccess) {
    auto end = steady_clock::now();
    auto elapsed = end - start;
    if (elapsed >= timeout) {
      return Status("Timed out");
    }

    size_t n = m_conn->Read(buffer, sizeof(buffer),
                            duration_cast<microseconds>(timeout - elapsed),
                            status, &error);
    if (n > 0) {
      message.insert(message.end(), &buffer[0], &buffer[n]);
    }
  }
  return error;
}

Status HdcClient::ReadResponseStatus(const char *expected) {
  unsigned len;
  auto error = ReadAllBytes(&len, sizeof(len));
  if (error.Fail()) {
    return error;
  }
  len = htonl(len);
  if (len >= MAX_PACKET_SIZE) {
    return Status("Read response packet too large: %u", len);
  }
  const unsigned vector_size = 128;
  llvm::SmallVector<char, vector_size> message(len + 1);
  error = ReadAllBytes(message.data(), len);
  if (error.Fail()) {
    return error;
  }

  message[len] = 0;
  if (expected == nullptr ||
      strncmp(message.data(), expected, strlen(expected))) {
    return Status("%s", message.data());
  }

  return error;
}

Status HdcClient::ReadAllBytes(void *buffer, size_t size) {
  return ::ReadAllBytes(*m_conn, buffer, size);
}

Status HdcClient::Shell(const char *command, milliseconds timeout,
                        std::string *output) {
  std::string cmd = "shell ";
  cmd += command;
  Status error = SendMessage(cmd);
  if (error.Fail()) {
    return error;
  }
  std::vector<char> message;
  error = ReadMessageStream(message, timeout);
  if (error.Fail()) {
    return error;
  }
  (*output) = std::string(message.data(), message.size());
  return error;
}
