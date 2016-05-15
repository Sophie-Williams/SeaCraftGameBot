/*
 * bot_main.cpp
 *
 *  Created on: 7 May 2016.
 *      Author: user
 */

#include <cstdlib>
#include <iostream>
#include <memory>
#include <random>
#include <vector>
#include <set>
#include <string>
#include <sstream>
#include <thread>

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

static const std::string kHttpLineEnd("\r\n");
static const std::string kHttpHeaderMessageDelimiter(kHttpLineEnd + kHttpLineEnd);
static const size_t kHttpLineEndSize = kHttpLineEnd.size();
static const size_t kHttpHeaderMessageDelimiterSize = kHttpHeaderMessageDelimiter.size();

class Bot {
public:
	Bot(const std::string &host, const int port, const int generator_seed,
			const size_t microseconds_time_interval) :
			server_socket_(socket(AF_INET, SOCK_STREAM, 0)), generator_(generator_seed), time_interval_(
					microseconds_time_interval) {
		Connect(host, port);
	}

	void Run();

private:
	enum {
		SHIPPING, WAITING, MAKING_STEP
	};

	enum {
		WATER, MISSED, SHIP_PIECE_OK, SHIP_PIECE_DEAD
	};

	enum {
		UNKNOWN, VERTICAL, HORIZONTAL
	};

	enum {
		LOST = -1, DRAW = 0, WON = 1
	};

	struct Step {
		int y_coord;
		int x_coord;

		Step(const int y_coord, const int x_coord) :
				y_coord(y_coord), x_coord(x_coord) {
		}
		Step(const std::initializer_list<int>& list) :
				y_coord(*list.begin()), x_coord(*(list.end() - 1)) {
		}
		bool operator<(const Step& other) const {
			return (y_coord < other.y_coord) || (y_coord == other.y_coord && x_coord < other.x_coord);
		}
	};


	void Connect(const std::string &host, int port);
	bool Send(const char *data, size_t sz) const;
	std::string RecieveAnswer() const;
	void SendGetQuery();
	void SendShipping() const;
	void SendStep(const Step& step) const;
	void SendToCheck() const;

	void Sleep() const {
		std::this_thread::sleep_for(std::chrono::microseconds(time_interval_));
	}

	void ToDoShipping();

	bool IsStepCorrect(const Step& step) const {
		return step.y_coord >= 0 && step.y_coord < kRowsCount && step.x_coord >= 0
				&& step.x_coord < kColumnsCount && enemy_ships_[step.y_coord][step.x_coord] == WATER;
	}

	Step TryToKillHalfShip() const;
	Step MakeStep();

	void FindOutHalfShipDirection(const Step& half_ship_dead_piece);
	void EraseFromPotentialSteps(const Step& useless_step);
	void ProcessFieldTwoHalfOrMissAnswer(const std::string& message_itself);
	void ProcessMissedAroundKilled(const std::set<Step>& killed);
	void ProcessFieldTwoKillAnswer(const std::string& message_itself);
	int ProcessAnswer(const std::string& answer);

	static bool ResolveHost(const std::string &host, int &addr);
	static std::string WrapMessage(const std::string& message);

//Skipping HTTP header, its end is indicated by an empty line
	static std::string SkipHeaders(const std::string& query);

	static constexpr size_t kRowsCount = 10;
	static constexpr size_t kColumnsCount = 10;
	static constexpr size_t kShipsBitsCount = kRowsCount * kColumnsCount;

	using FieldRow = std::vector<size_t>;
	std::vector<FieldRow> my_ships_;
	std::vector<FieldRow> enemy_ships_;

	int server_socket_;
	size_t status = SHIPPING;
	std::string login_;
	std::set<Step> potential_steps_;
	std::set<Step> current_half_ship_pieces_;
	size_t current_half_ship_direction_ = UNKNOWN;
	std::mt19937 generator_;
	const size_t time_interval_;
};

int main(int argc, char** argv) {
	static const std::string host = "127.0.0.1";

	if (argc < 2) {
		std::printf(
				"Usage: %s port_to_connect generator_seed(optional) time_interval_between_bot's_actions(in microseconds, optional)\n",
				argv[0]);
		return 1;
	}

	int port_to_connect = std::atoi(argv[1]);
	int generator_seed = argc >= 3 ? std::atoi(argv[2]) : 838;
	int time_interval = argc >= 4 ? std::atoi(argv[3]) : 1000;
	Bot bot(host, port_to_connect, generator_seed, time_interval);
	bot.Run();

	return 0;
}

void Bot::Run() {
	SendGetQuery();
	std::string answer;
	do {
		Sleep();
		switch (status) {
		case SHIPPING:
			ToDoShipping();
			SendShipping();
			status = WAITING;
			break;
		case WAITING:
			SendToCheck();
			break;
		case MAKING_STEP:
			Step step = MakeStep();
			SendStep(step);
			break;
		}
		answer = RecieveAnswer();
	} while (ProcessAnswer(answer) == DRAW);
}

bool Bot::ResolveHost(const std::string &host, int &addr) {
	hostent *ent = gethostbyname(host.c_str());
	if (ent == nullptr)
		return false;
	for (size_t i = 0; ent->h_addr_list[i]; ++i) {
		addr = *reinterpret_cast<int**>(ent->h_addr_list)[i];
		return true;
	}
	return false;
}

void Bot::Connect(const std::string &host, int port) {
	int addr;
	if (!ResolveHost(host, addr))
		throw std::runtime_error("can't resolve host");
	sockaddr_in address;
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = addr;
	if (connect(server_socket_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
		throw std::runtime_error("can't connect");
	}
}

bool Bot::Send(const char *data, size_t sz) const {
	for (; sz > 0;) {
		int res = send(server_socket_, data, sz, 0);
		if (res == 0) {
			return false;
		}
		if (res < 0) {
			throw std::runtime_error("error in send");
		}
		data += res;
		sz -= res;
	}
	return true;
}

std::string Bot::RecieveAnswer() const {
	static const std::string kContentLengthHeader("Content-Length: ");
	static const size_t kContentLengthHeaderLen = kContentLengthHeader.size();
	std::string answer;
	size_t size_to_recv = 0;

	for (bool content_length_found = false; !content_length_found;) {
		char buf[1000000] = "";
		recv(server_socket_, buf, sizeof(buf), 0);
		answer.append(buf);
		size_t content_length_header_begpos = answer.find(kContentLengthHeader);
		size_t http_header_endpos = answer.find(kHttpHeaderMessageDelimiter);
		if (content_length_header_begpos != std::string::npos
				&& http_header_endpos != std::string::npos) {
			content_length_found = true;
			const size_t content_length_endpos = answer.find(kHttpLineEnd, content_length_header_begpos);
			const size_t content_length_begpos = content_length_header_begpos + kContentLengthHeaderLen;
			const size_t content_length =
					std::atoi(
							answer.substr(content_length_begpos, content_length_endpos - (content_length_begpos)).c_str());
			const size_t read_content_length = answer.size()
					- (http_header_endpos + kHttpHeaderMessageDelimiterSize);
			size_to_recv = content_length - read_content_length;
		}
	}

	for (; size_to_recv > 0;) {
		char buf[1000000] = "";
		int res = recv(server_socket_, buf, sizeof(buf), 0);
		answer.append(buf);
		size_to_recv -= res;
	}

	return answer;
}

void Bot::SendGetQuery() {
	static const std::string kGetQery = "GET / HTTP" + kHttpHeaderMessageDelimiter;
	static const std::string kLoginHeader("your_login\" class=\"");
	static constexpr size_t kLoginLength = 3;
	Send(kGetQery.c_str(), kGetQery.size());
	std::string html_page = RecieveAnswer();
	const size_t login_beg_pos = html_page.find(kLoginHeader) + kLoginHeader.size();
	login_ = html_page.substr(login_beg_pos, kLoginLength);
}

std::string Bot::WrapMessage(const std::string& message) {
	std::stringstream query_stream;
	query_stream << "POST" << kHttpLineEnd << "Content-Length: " << message.size()
			<< kHttpHeaderMessageDelimiter << message;
	return query_stream.str();
}

void Bot::SendShipping() const {
	std::string message = login_ + ":shipping:";
	message.reserve(kShipsBitsCount + message.size());
	for (int y_coord = 0; y_coord != kRowsCount; ++y_coord) {
		for (int x_coord = 0; x_coord != kColumnsCount; ++x_coord) {
			message.push_back(my_ships_[y_coord][x_coord] == WATER ? '0' : '1');
		}
	}
	std::string query = WrapMessage(message);

	Send(query.c_str(), query.size());
}

void Bot::SendStep(const Step& step) const {
	std::stringstream message_stream;
	message_stream << login_ << ":step:" << step.y_coord + 1 << ":" << step.x_coord + 1;
	std::string query = WrapMessage(message_stream.str());

	Send(query.c_str(), query.size());
	std::stringstream console_out_stream;
	console_out_stream << "My step is " << step.y_coord + 1 << ":" << step.x_coord + 1 << " and I ";
	std::cout << console_out_stream.str();
}

void Bot::SendToCheck() const {
	static const std::string kCheckMessage(login_ + ":");
	static const std::string kCheckQuery = WrapMessage(kCheckMessage);

	Send(kCheckQuery.c_str(), kCheckQuery.size());
}

void Bot::ToDoShipping() {
	static const std::string kShips =
			"1111000000000000000011100011100000000000101010000010101000010000000000000100010000000000000000010000";
	auto ships_iterator = kShips.begin();
	my_ships_.resize(kRowsCount);
	enemy_ships_.resize(kRowsCount);
	for (int y_coord = 0; y_coord != kRowsCount; ++y_coord) {
		my_ships_[y_coord].resize(kColumnsCount, WATER);
		enemy_ships_[y_coord].resize(kColumnsCount, WATER);
		for (int x_coord = 0; x_coord != kColumnsCount; ++x_coord, ++ships_iterator) {
			potential_steps_.emplace(y_coord, x_coord);
			if (*ships_iterator == '1') {
				my_ships_[y_coord][x_coord] = SHIP_PIECE_OK;
			}
		}
	}
}

Bot::Step Bot::TryToKillHalfShip() const {
	Step half_ship_piece = *current_half_ship_pieces_.begin();
	const int dead_piece_y = half_ship_piece.y_coord;
	const int dead_piece_x = half_ship_piece.x_coord;
	if (current_half_ship_direction_ == HORIZONTAL) {
		for (int x_coord = dead_piece_x + 1;
				x_coord != 10 && enemy_ships_[dead_piece_y][x_coord] != MISSED; ++x_coord) {
			if (enemy_ships_[dead_piece_y][x_coord] == WATER) {
				return {dead_piece_y, x_coord};
			}
		}
		for (int x_coord = dead_piece_x - 1;
				x_coord >= 0 && enemy_ships_[dead_piece_y][x_coord] != MISSED; --x_coord) {
			if (enemy_ships_[dead_piece_y][x_coord] == WATER) {
				return {dead_piece_y, x_coord};
			}
		}
	} else if (current_half_ship_direction_ == VERTICAL) {
		for (int y_coord = dead_piece_y + 1;
				y_coord != 10 && enemy_ships_[y_coord][dead_piece_x] != MISSED; ++y_coord) {
			if (enemy_ships_[y_coord][dead_piece_x] == WATER) {
				return {y_coord, dead_piece_x};
			}
		}
		for (int y_coord = dead_piece_y - 1;
				y_coord >= 0 && enemy_ships_[y_coord][dead_piece_x] != MISSED; --y_coord) {
			if (enemy_ships_[y_coord][dead_piece_x] == WATER) {
				return {y_coord, dead_piece_x};
			}
		}
	} else if (current_half_ship_direction_ == UNKNOWN) {
		std::vector<Step> step_candidates = { { dead_piece_y - 1, dead_piece_x }, { dead_piece_y,
				dead_piece_x + 1 }, { dead_piece_y + 1, dead_piece_x }, { dead_piece_y, dead_piece_x - 1 } };
		for (const Step& step_candidate : step_candidates) {
			if (IsStepCorrect(step_candidate)) {
				return step_candidate;
			}
		}
	} else {
		std::cout << "Problem in TryToKillHalfShip" << std::endl;
		return {0,0};
	}
}

Bot::Step Bot::MakeStep() {
	if (!current_half_ship_pieces_.empty()) {
		return TryToKillHalfShip();
	}

	//  There is no half ship
	std::uniform_int_distribution<size_t> potential_steps_indices(0, potential_steps_.size() - 1);
	const size_t potential_step_index = potential_steps_indices(generator_);
	auto potential_step_iter = potential_steps_.begin();
	for (size_t i = 0; i != potential_step_index; ++i) {
		++potential_step_iter;
	}
	return *potential_step_iter;
}

//Skipping HTTP header, its end is indicated by an empty line
std::string Bot::SkipHeaders(const std::string& query) {
	int begposition_of_message_itself = query.find(kHttpHeaderMessageDelimiter)
			+ kHttpHeaderMessageDelimiterSize;
	return query.substr(begposition_of_message_itself);

}

void Bot::FindOutHalfShipDirection(const Step& half_ship_dead_piece) {
	const size_t y_coord = half_ship_dead_piece.y_coord;
	const size_t x_coord = half_ship_dead_piece.x_coord;
	if (current_half_ship_pieces_.find(Step(y_coord - 1, x_coord)) != current_half_ship_pieces_.end()
			|| current_half_ship_pieces_.find(Step(y_coord + 1, x_coord))
					!= current_half_ship_pieces_.end()) {
		current_half_ship_direction_ = VERTICAL;
	} else if (current_half_ship_pieces_.find(Step(y_coord, x_coord - 1))
			!= current_half_ship_pieces_.end()
			|| current_half_ship_pieces_.find(Step(y_coord, x_coord + 1))
					!= current_half_ship_pieces_.end()) {
		current_half_ship_direction_ = HORIZONTAL;
	} else {
		std::cout << "Problem in FindOutHalfShipDirection" << std::endl;
	}
}

void Bot::EraseFromPotentialSteps(const Step& useless_step) {
	auto iter = potential_steps_.find(useless_step);
	if (iter != potential_steps_.end()) {
		potential_steps_.erase(iter);
	}
}

void Bot::ProcessFieldTwoHalfOrMissAnswer(const std::string& message_itself) {
	std::stringstream stream(message_itself.substr(12));
	size_t y_coord, x_coord;
	char ch;
	stream >> y_coord >> ch >> x_coord;
	--y_coord;
	--x_coord;
	EraseFromPotentialSteps(Step(y_coord, x_coord));
	if (message_itself.find("half") != std::string::npos) {
		enemy_ships_[y_coord][x_coord] = SHIP_PIECE_DEAD;
		current_half_ship_pieces_.emplace(y_coord, x_coord);
		if (current_half_ship_direction_ == UNKNOWN && current_half_ship_pieces_.size() == 2) {
			FindOutHalfShipDirection(Step(y_coord, x_coord));
		}
		std::cout << "made ship-piece dead" << std::endl;
	} else if (message_itself.find("miss") != std::string::npos) {
		enemy_ships_[y_coord][x_coord] = MISSED;
		status = WAITING;
		std::cout << "missed" << std::endl;
	}
}

void Bot::ProcessMissedAroundKilled(const std::set<Step>& killed) {
	auto left_up = killed.begin();
	auto right_down = --killed.end();
	for (int i = std::max(0, left_up->y_coord - 1); i <= std::min(9, right_down->y_coord + 1); ++i) {
		if (left_up->x_coord - 1 >= 0) {
			enemy_ships_[i][left_up->x_coord - 1] = MISSED;
			EraseFromPotentialSteps(Step(i, left_up->x_coord - 1));
		}
		if (right_down->x_coord + 1 <= 9) {
			enemy_ships_[i][right_down->x_coord + 1] = MISSED;
			EraseFromPotentialSteps(Step(i, right_down->x_coord + 1));
		}
	}

	for (int i = std::max(0, left_up->x_coord - 1); i <= std::min(9, right_down->x_coord + 1); ++i) {
		if (left_up->y_coord - 1 >= 0) {
			enemy_ships_[left_up->y_coord - 1][i] = MISSED;
			EraseFromPotentialSteps(Step(left_up->y_coord - 1, i));
		}
		if (right_down->y_coord + 1 <= 9) {
			enemy_ships_[right_down->y_coord + 1][i] = MISSED;
			EraseFromPotentialSteps(Step(right_down->y_coord + 1, i));
		}
	}
}

void Bot::ProcessFieldTwoKillAnswer(const std::string& message_itself) {
	std::stringstream stream(message_itself.substr(11));
	std::set<Step> killed;
	while (!stream.eof()) {
		size_t y_coord, x_coord;
		char ch;
		stream >> ch >> y_coord >> ch >> x_coord;
		--y_coord;
		--x_coord;
		enemy_ships_[y_coord][x_coord] = SHIP_PIECE_DEAD;
		EraseFromPotentialSteps(Step(y_coord, x_coord));
		killed.emplace(y_coord, x_coord);
	}

	ProcessMissedAroundKilled(killed);
	current_half_ship_pieces_.clear();
	current_half_ship_direction_ = UNKNOWN;

	std::cout << "killed " << stream.str() <<std::endl;
}

int Bot::ProcessAnswer(const std::string& answer) {
	std::string message_itself = SkipHeaders(answer);
	if (message_itself == "go1") {
		status = MAKING_STEP;
	} else if (message_itself.find("field1:miss:") != std::string::npos) {
		status = MAKING_STEP;
	} else if (message_itself.find("field2:") != std::string::npos
			&& message_itself.find("kill") == std::string::npos) {
		ProcessFieldTwoHalfOrMissAnswer(message_itself);
	} else if (message_itself.find("field2:kill:") != std::string::npos) {
		ProcessFieldTwoKillAnswer(message_itself);
	} else if (message_itself.find("lost") != std::string::npos) {
		std::cout << "I lost" << std::endl;
		return LOST;
	} else if (message_itself.find("won") != std::string::npos) {
		std::cout << "I won" << std::endl;
		return WON;
	}

	return DRAW;
}
