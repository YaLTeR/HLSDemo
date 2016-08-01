#pragma once
#include <cstddef>
#include <string>
#include <vector>

enum class DemoFrameType : uint8_t {
	STARTUP_PACKET = 1,
	NETWORK_PACKET = 2,
	JUMPTIME = 3,
	CONSOLE_COMMAND = 4,
	USERCMD = 5,
	STRINGTABLES = 6,
	NETWORK_DATA_TABLE = 7,
	NEXT_SECTION = 8
};

struct DemoFrame {
	DemoFrameType type;
	float time;
	int32_t frame;

	virtual ~DemoFrame() {}
};

struct ConsoleCommandFrame : DemoFrame {
	std::string command;
};

struct StringTablesFrame : DemoFrame {
	std::vector<unsigned char> data;
};

struct NetworkDataTableFrame : DemoFrame {
	std::vector<unsigned char> data;
};

struct UserCmdFrame : DemoFrame {
	int32_t outgoing_sequence;
	int32_t slot;
	std::vector<unsigned char> data;
};

// Otherwise, netmsg.
struct NetMsgFrame : DemoFrame {
	struct {
		int32_t flags;
		
		float viewOrigin[3];
		float viewAngles[3];
		float localViewAngles[3];

		float viewOrigin2[3];
		float viewAngles2[3];
		float localViewAngles2[3];
	} DemoInfo;

	int32_t incoming_sequence;
	int32_t incoming_acknowledged;
	int32_t incoming_reliable_acknowledged;
	int32_t incoming_reliable_sequence;
	int32_t outgoing_sequence;
	int32_t reliable_sequence;
	int32_t last_reliable_sequence;

	std::vector<unsigned char> msg;
};
