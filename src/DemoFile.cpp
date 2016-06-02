#include <cassert>
#include <chrono>
#include <codecvt>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <locale>
#include <unordered_map>
#include <memory>
#include <vector>

#include "DemoFile.hpp"
#include "DemoFrame.hpp"

enum {
	HEADER_SIZE = 540,
	HEADER_SIGNATURE_CHECK_SIZE = 6,
	HEADER_SIGNATURE_SIZE = 8,
	HEADER_MAPNAME_SIZE = 260,
	HEADER_GAMEDIR_SIZE = 260,

	MIN_DIR_ENTRY_COUNT = 1,
	MAX_DIR_ENTRY_COUNT = 1024,
	DIR_ENTRY_SIZE = 20,

	MIN_FRAME_SIZE = 9,
	FRAME_CONSOLE_COMMAND_SIZE = 4,
	FRAME_CONSOLE_COMMAND_MAX_SIZE = 2048,
	FRAME_USERCMD_SIZE = 10,
	FRAME_USERCMD_DATA_MAX_SIZE = 1024,
	FRAME_STRINGTABLES_SIZE = 4,
	FRAME_NETMSG_SIZE = 108,
	FRAME_NETMSG_MIN_MESSAGE_LENGTH = 0,
	FRAME_NETMSG_MAX_MESSAGE_LENGTH = 80032
};

template<typename T>
static void read_object(std::ifstream& i, T& obj)
{
	i.read(reinterpret_cast<char*>(&obj), sizeof(T));
}

template<typename T>
static void read_objects(std::ifstream& i, std::vector<T>& objs)
{
	i.read(reinterpret_cast<char*>(objs.data()), objs.size() * sizeof(T));
}

template<typename T>
static void write_object(std::ofstream& o, const T& obj)
{
	o.write(reinterpret_cast<const char*>(&obj), sizeof(T));
}

template<typename T>
static void write_objects(std::ofstream& o, const std::vector<T>& objs)
{
	o.write(reinterpret_cast<const char*>(objs.data()), objs.size() * sizeof(T));
}

static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> string_converter;
static std::wstring utf8_to_utf16(const std::string& str)
{
	return string_converter.from_bytes(str);
}

static std::string utf16_to_utf8(const std::wstring& str)
{
	return string_converter.to_bytes(str);
}

#ifdef _WIN32
#define utf8_filename(str) utf8_to_utf16(str)
#define utf16_filename(str) str
#else
#define utf8_filename(str) str
#define utf16_filename(str) utf16_to_utf8(str)
#endif

DemoDirectoryEntry::DemoDirectoryEntry(const DemoDirectoryEntry& e)
	: type(e.type)
	, playbackTime(e.playbackTime)
	, frameCount(e.frameCount)
	, offset(e.offset)
	, fileLength(e.fileLength)
{
	frames.reserve(e.frames.size());
	for (const auto& frame : e.frames) {
		#define PUSH_COPY(class) \
			frames.emplace_back(new class(*static_cast<class*>(frame.get())))

		switch (frame->type) {
			case DemoFrameType::JUMPTIME:
			case DemoFrameType::NEXT_SECTION:
				frames.emplace_back(new DemoFrame(*frame));
				break;

			case DemoFrameType::CONSOLE_COMMAND:
				PUSH_COPY(ConsoleCommandFrame);
				break;

			case DemoFrameType::USERCMD:
				PUSH_COPY(UserCmdFrame);
				break;

			case DemoFrameType::STRINGTABLES:
				PUSH_COPY(StringTablesFrame);
				break;
				
			default:
				PUSH_COPY(NetMsgFrame);
				break;
		}

		#undef PUSH_COPY
	}
}

DemoDirectoryEntry& DemoDirectoryEntry::operator= (DemoDirectoryEntry e)
{
	swap(e);
	return *this;
}

void DemoDirectoryEntry::swap(DemoDirectoryEntry& e)
{
	std::swap(type, e.type);
	std::swap(playbackTime, e.playbackTime);
	std::swap(frameCount, e.frameCount);
	std::swap(offset, e.offset);
	std::swap(fileLength, e.fileLength);
	std::swap(frames, e.frames);
}

DemoFile::DemoFile(std::string filename_, bool read_frames)
	: filename(std::move(filename_))
	, readFrames(false)
{
	ConstructorInternal(std::ifstream(utf8_filename(filename), std::ios::binary), read_frames);
}

DemoFile::DemoFile(std::wstring filename_, bool read_frames)
	: filename(utf16_to_utf8(filename_))
	, readFrames(false)
{
	ConstructorInternal(std::ifstream(utf16_filename(filename_), std::ios::binary), read_frames);
}

void DemoFile::ConstructorInternal(std::ifstream demo, bool read_frames)
{
	if (!demo)
		throw std::runtime_error("Error opening the demo file.");

	demo.seekg(0, std::ios::end);
	size_t demoSize = demo.tellg();
	if (demoSize < HEADER_SIZE)
		throw std::runtime_error("Invalid demo file (the size is too small).");

	demo.seekg(0, std::ios::beg);
	char signature[HEADER_SIGNATURE_CHECK_SIZE];
	demo.read(signature, sizeof(signature));
	if (std::memcmp(signature, "HLDEMO", sizeof(signature)))
		throw std::runtime_error("Invalid demo file (signature doesn't match).");

	ReadHeader(demo);
	ReadDirectory(demo, demoSize);

	if (read_frames)
		ReadFramesInternal(demo, demoSize);
}

void DemoFile::ReadHeader(std::ifstream& demo)
{
	demo.seekg(HEADER_SIGNATURE_SIZE, std::ios::beg);
	read_object(demo, header.demoProtocol);
	read_object(demo, header.netProtocol);

	std::vector<char> mapNameBuf(HEADER_MAPNAME_SIZE);
	read_objects(demo, mapNameBuf);
	mapNameBuf.push_back('\0');
	header.mapName = mapNameBuf.data();

	std::vector<char> gameDirBuf(HEADER_GAMEDIR_SIZE);
	read_objects(demo, gameDirBuf);
	gameDirBuf.push_back('\0');
	header.gameDir = gameDirBuf.data();

	read_object(demo, header.directoryOffset);
}

void DemoFile::ReadDirectory(std::ifstream& demo, size_t demoSize)
{
	if (header.directoryOffset < 0 || (demoSize - 4) < static_cast<size_t>(header.directoryOffset))
		throw std::runtime_error("Error parsing the demo directory: invalid directory offset.");

	demo.seekg(header.directoryOffset, std::ios::beg);
	int32_t dirEntryCount;
	read_object(demo, dirEntryCount);
	if (dirEntryCount < MIN_DIR_ENTRY_COUNT
		|| dirEntryCount > MAX_DIR_ENTRY_COUNT
		|| (demoSize - demo.tellg()) < static_cast<size_t>(dirEntryCount * DIR_ENTRY_SIZE))
		throw std::runtime_error("Error parsing the demo directory: invalid directory entry count.");

	directoryEntries.clear();
	directoryEntries.reserve(dirEntryCount);
	for (auto i = 0; i < dirEntryCount; ++i) {
		DemoDirectoryEntry entry;

		read_object(demo, entry.type);
		read_object(demo, entry.playbackTime);
		read_object(demo, entry.frameCount);
		read_object(demo, entry.offset);
		read_object(demo, entry.fileLength);

		directoryEntries.push_back(entry);
	}
}

bool DemoFile::IsValidDemoFile(const std::string& filename)
{
	return IsValidDemoFileInternal(std::ifstream(utf8_filename(filename), std::ios::binary));
}

bool DemoFile::IsValidDemoFile(const std::wstring& filename)
{
	return IsValidDemoFileInternal(std::ifstream(utf16_filename(filename), std::ios::binary));
}

bool DemoFile::IsValidDemoFileInternal(std::ifstream in)
{
	if (!in)
		throw std::runtime_error("Error opening the file.");

	in.seekg(0, std::ios::end);
	auto size = in.tellg();
	if (size < HEADER_SIZE)
		return false;

	in.seekg(0, std::ios::beg);
	char signature[HEADER_SIGNATURE_CHECK_SIZE];
	in.read(signature, sizeof(signature));
	if (std::memcmp(signature, "HLDEMO", sizeof(signature)))
		return false;

	return true;
}

void DemoFile::ReadFrames()
{
	if (!readFrames) {
		DemoFile demo(filename, true);
		swap(demo);
	}
}

void DemoFile::ReadFramesInternal(std::ifstream& demo, size_t demoSize)
{
	assert(!readFrames);

	if (header.demoProtocol != 2) {
		throw std::runtime_error("Only demo protocol 2 is supported.");
	}

	// On any error, just skip to the next entry.
	for (auto& entry : directoryEntries) {
		auto offset = entry.offset;
		if (entry.offset < 0 || demoSize < static_cast<size_t>(entry.offset)) {
			// Invalid offset.
			continue;
		}

		demo.seekg(offset, std::ios::beg);

		bool stop = false;
		while (!stop) {
			if (demoSize - demo.tellg() < MIN_FRAME_SIZE) {
				// Unexpected EOF.
				break;
			}

			DemoFrame frame;
			read_object(demo, frame.type);
			read_object(demo, frame.time);
			read_object(demo, frame.frame);

			switch (frame.type) {
			case DemoFrameType::JUMPTIME:
			{
				entry.frames.emplace_back(new DemoFrame(std::move(frame)));
			}
				break;

			case DemoFrameType::CONSOLE_COMMAND:
			{
				if (demoSize - demo.tellg() < FRAME_CONSOLE_COMMAND_SIZE) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				ConsoleCommandFrame f;
				f.type = frame.type;
				f.time = frame.time;
				f.frame = frame.frame;

				int32_t length;
				read_object(demo, length);

				if (length < 0 || length > FRAME_CONSOLE_COMMAND_MAX_SIZE || demoSize - demo.tellg() < static_cast<size_t>(length)) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				std::vector<char> commandBuf(length);
				read_objects(demo, commandBuf);
				commandBuf.push_back('\0');
				f.command = commandBuf.data();

				entry.frames.emplace_back(new ConsoleCommandFrame(std::move(f)));
			}
				break;

			case DemoFrameType::USERCMD:
			{
				if (demoSize - demo.tellg() < FRAME_USERCMD_SIZE) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				UserCmdFrame f;
				f.type = frame.type;
				f.time = frame.time;
				f.frame = frame.frame;

				read_object(demo, f.outgoing_sequence);
				read_object(demo, f.slot);

				uint16_t length;
				read_object(demo, length);

				if (length > FRAME_USERCMD_DATA_MAX_SIZE || demoSize - demo.tellg() < static_cast<size_t>(length)) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				f.data.resize(length);
				read_objects(demo, f.data);

				entry.frames.emplace_back(new UserCmdFrame(std::move(f)));
			}
				break;

			case DemoFrameType::STRINGTABLES:
			{
				if (demoSize - demo.tellg() < FRAME_STRINGTABLES_SIZE) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				StringTablesFrame f;
				f.type = frame.type;
				f.time = frame.time;
				f.frame = frame.frame;

				int32_t length;
				read_object(demo, length);

				if (length < 0 || demoSize - demo.tellg() < static_cast<size_t>(length)) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				f.data.resize(length);
				read_objects(demo, f.data);

				entry.frames.emplace_back(new StringTablesFrame(std::move(f)));
			}
				break;

			case DemoFrameType::NEXT_SECTION:
			{
				entry.frames.emplace_back(new DemoFrame(std::move(frame)));

				stop = true;
			}
				break;

			default:
			{
				if (demoSize - demo.tellg() < FRAME_NETMSG_SIZE) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				NetMsgFrame f;
				f.type = frame.type;
				f.time = frame.time;
				f.frame = frame.frame;

				read_object(demo, f.DemoInfo.flags);

				read_object(demo, f.DemoInfo.viewOrigin[0]);
				read_object(demo, f.DemoInfo.viewOrigin[1]);
				read_object(demo, f.DemoInfo.viewOrigin[2]);
				read_object(demo, f.DemoInfo.viewAngles[0]);
				read_object(demo, f.DemoInfo.viewAngles[1]);
				read_object(demo, f.DemoInfo.viewAngles[2]);
				read_object(demo, f.DemoInfo.localViewAngles[0]);
				read_object(demo, f.DemoInfo.localViewAngles[1]);
				read_object(demo, f.DemoInfo.localViewAngles[2]);

				read_object(demo, f.DemoInfo.viewOrigin2[0]);
				read_object(demo, f.DemoInfo.viewOrigin2[1]);
				read_object(demo, f.DemoInfo.viewOrigin2[2]);
				read_object(demo, f.DemoInfo.viewAngles2[0]);
				read_object(demo, f.DemoInfo.viewAngles2[1]);
				read_object(demo, f.DemoInfo.viewAngles2[2]);
				read_object(demo, f.DemoInfo.localViewAngles2[0]);
				read_object(demo, f.DemoInfo.localViewAngles2[1]);
				read_object(demo, f.DemoInfo.localViewAngles2[2]);

				read_object(demo, f.incoming_sequence);
				read_object(demo, f.incoming_acknowledged);
				read_object(demo, f.incoming_reliable_acknowledged);
				read_object(demo, f.incoming_reliable_sequence);
				read_object(demo, f.outgoing_sequence);
				read_object(demo, f.reliable_sequence);
				read_object(demo, f.last_reliable_sequence);

				int32_t length;
				read_object(demo, length);
				if (length < FRAME_NETMSG_MIN_MESSAGE_LENGTH
					|| length > FRAME_NETMSG_MAX_MESSAGE_LENGTH) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				if (demoSize - demo.tellg() < static_cast<size_t>(length)) {
					// Unexpected EOF.
					stop = true;
					break;
				}

				f.msg.resize(length);
				read_objects(demo, f.msg);

				entry.frames.emplace_back(new NetMsgFrame(std::move(f)));
			}
				break;
			}
		}
	}

	readFrames = true;
}

void DemoFile::Save() const
{
	DemoFile::Save(filename);
}

void DemoFile::Save(const std::string& filename) const
{
	DemoFile::SaveInternal(std::ofstream(utf8_filename(filename), std::ios::trunc | std::ios::binary));
}

void DemoFile::Save(const std::wstring& filename) const
{
	DemoFile::SaveInternal(std::ofstream(utf16_filename(filename), std::ios::trunc | std::ios::binary));
}

void DemoFile::SaveInternal(std::ofstream o) const
{
	if (!o)
		throw std::runtime_error("Error opening the output file.");

	char signature[] = { 'H', 'L', 'D', 'E', 'M', 'O', '\0', '\0' };
	o.write(signature, sizeof(signature));
	write_object(o, header.demoProtocol);
	write_object(o, header.netProtocol);

	std::vector<char> mapNameBuf;
	std::copy(std::begin(header.mapName), std::end(header.mapName), std::back_inserter(mapNameBuf));
	mapNameBuf.push_back('\0');
	mapNameBuf.resize(HEADER_MAPNAME_SIZE);
	write_objects(o, mapNameBuf);

	std::vector<char> gameDirBuf;
	std::copy(std::begin(header.gameDir), std::end(header.gameDir), std::back_inserter(gameDirBuf));
	gameDirBuf.push_back('\0');
	gameDirBuf.resize(HEADER_GAMEDIR_SIZE);
	write_objects(o, gameDirBuf);

	// Directory offset goes here.
	auto dirOffsetPos = o.tellp();
	o.seekp(4, std::ios::cur);

	std::unordered_map<const DemoDirectoryEntry*, int32_t> new_offsets;

	for (const auto& entry : directoryEntries) {
		new_offsets[&entry] = static_cast<int32_t>(o.tellp());

		// We need to write at least one NextSectionFrame, otherwise
		// the engine might break trying to play back the demo.
		bool wroteNextSection = false;
		for (const auto& frame : entry.frames) {
			write_object(o, frame->type);
			write_object(o, frame->time);
			write_object(o, frame->frame);

			switch (frame->type) {
			case DemoFrameType::JUMPTIME:
				// No extra info.
				break;

			case DemoFrameType::CONSOLE_COMMAND:
			{
				auto f = reinterpret_cast<ConsoleCommandFrame*>(frame.get());

				write_object(o, static_cast<int32_t>(f->command.size() + 1));

				std::vector<char> commandBuf;
				std::copy(std::begin(f->command), std::end(f->command), std::back_inserter(commandBuf));
				commandBuf.push_back('\0');
				write_objects(o, commandBuf);
			}
				break;

			case DemoFrameType::USERCMD:
			{
				auto f = reinterpret_cast<UserCmdFrame*>(frame.get());

				write_object(o, f->outgoing_sequence);
				write_object(o, f->slot);
				write_object(o, static_cast<uint16_t>(f->data.size()));
				write_objects(o, f->data);
			}
				break;

			case DemoFrameType::STRINGTABLES:
			{
				auto f = reinterpret_cast<StringTablesFrame*>(frame.get());

				write_object(o, static_cast<int32_t>(f->data.size()));
				write_objects(o, f->data);
			}
				break;

			case DemoFrameType::NEXT_SECTION:
				// No extra info.
				wroteNextSection = true;
				break;

			default:
			{
				auto f = reinterpret_cast<NetMsgFrame*>(frame.get());
				
				write_object(o, f->DemoInfo.flags);

				write_object(o, f->DemoInfo.viewOrigin[0]);
				write_object(o, f->DemoInfo.viewOrigin[1]);
				write_object(o, f->DemoInfo.viewOrigin[2]);
				write_object(o, f->DemoInfo.viewAngles[0]);
				write_object(o, f->DemoInfo.viewAngles[1]);
				write_object(o, f->DemoInfo.viewAngles[2]);
				write_object(o, f->DemoInfo.localViewAngles[0]);
				write_object(o, f->DemoInfo.localViewAngles[1]);
				write_object(o, f->DemoInfo.localViewAngles[2]);

				write_object(o, f->DemoInfo.viewOrigin2[0]);
				write_object(o, f->DemoInfo.viewOrigin2[1]);
				write_object(o, f->DemoInfo.viewOrigin2[2]);
				write_object(o, f->DemoInfo.viewAngles2[0]);
				write_object(o, f->DemoInfo.viewAngles2[1]);
				write_object(o, f->DemoInfo.viewAngles2[2]);
				write_object(o, f->DemoInfo.localViewAngles2[0]);
				write_object(o, f->DemoInfo.localViewAngles2[1]);
				write_object(o, f->DemoInfo.localViewAngles2[2]);

				write_object(o, f->incoming_sequence);
				write_object(o, f->incoming_acknowledged);
				write_object(o, f->incoming_reliable_acknowledged);
				write_object(o, f->incoming_reliable_sequence);
				write_object(o, f->outgoing_sequence);
				write_object(o, f->reliable_sequence);
				write_object(o, f->last_reliable_sequence);

				write_object(o, static_cast<int32_t>(f->msg.size()));
				write_objects(o, f->msg);
			}
				break;
			}
		}

		if (!wroteNextSection) {
			DemoFrame f;
			f.type = DemoFrameType::NEXT_SECTION;
			f.time = 0;
			f.frame = 0;

			write_object(o, f.type);
			write_object(o, f.time);
			write_object(o, f.frame);
		}
	}

	auto dirOffset = o.tellp();
	write_object(o, static_cast<int32_t>(directoryEntries.size()));
	for (const auto& entry : directoryEntries) {
		write_object(o, entry.type);
		write_object(o, entry.playbackTime);
		write_object(o, entry.frameCount);
		write_object(o, new_offsets.at(&entry));
		write_object(o, entry.fileLength);
	}

	o.seekp(dirOffsetPos, std::ios::beg);
	write_object(o, static_cast<int32_t>(dirOffset));

	o.close();
}

void DemoFile::swap(DemoFile& f)
{
	std::swap(filename, f.filename);
	std::swap(readFrames, f.readFrames);
	std::swap(header, f.header);
	std::swap(directoryEntries, f.directoryEntries);
}
