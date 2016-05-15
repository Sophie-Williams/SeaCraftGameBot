/*
 * bot_main.cpp
 *
 *  Created on: 7 ��� 2016 �.
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

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>

static const std::string kHttpLineEnd("\r\n");
static const std::string kHttpHeaderMessageDelimiter(kHttpLineEnd + kHttpLineEnd);
static const size_t kHttpLineEndSize = 2;
static const size_t kHttpHeaderMessageDelimiterSize = 4;

class Bot {
public:
	using Step = std::pair<int, int>;

	Bot() = default;

	enum {
		SHIPPING, WAITING, MAKING_STEP
	};

	enum {
		WATER, MISSED, SHIP_PIECE_OK, SHIP_PIECE_DEAD
	};

	enum {
		UNKNOWN, VERTICAL, HORIZONTAL
	};

	static bool ResolveHost(const std::string &host, int &addr) {
		hostent *ent = gethostbyname(host.c_str());
		if (ent == nullptr)
			return false;
		for (size_t i = 0; ent->h_addr_list[i]; ++i) {
			addr = *reinterpret_cast<int**>(ent->h_addr_list)[i];
			return true;
		}
		return false;
	}

	void Connect(const std::string &host, int port) {
		int addr;
		if (!ResolveHost(host, addr))
			throw std::runtime_error("can't resolve host");
		sockaddr_in address;
		address.sin_family = AF_INET;
		address.sin_port = htons(port);
		address.sin_addr.s_addr = addr;
		if (connect(server_socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
			throw std::runtime_error("can't connect");
	}

	bool Send(const char *data, size_t sz) const {
		for (; sz > 0;) {
			int res = send(server_socket, data, sz, 0);
			if (res == 0)
				return false;
			if (res < 0)
				throw std::runtime_error("error in send");
			data += res;
			sz -= res;
		}
		return true;
	}

	void ToDoShipping() {
		static const std::string kShips =
				"1111000000000000000011100011100000000000101010000010101000010000000000000100010000000000000000010000";
		auto buf = kShips.begin();
		ships_.resize(10);
		enemy_ships_.resize(10);
		for (int y_coord = 0; y_coord != 10; ++y_coord) {
			ships_[y_coord].resize(10, WATER);
			enemy_ships_[y_coord].resize(10);
			for (int x_coord = 0; x_coord != 10; ++x_coord, ++buf) {
				potential_steps_.emplace(y_coord, x_coord);
				if (*buf == '0') {
					ships_[y_coord][x_coord] = WATER;
				} else if (*buf == '1') {
					ships_[y_coord][x_coord] = SHIP_PIECE_OK;
				} else {
					std::cout << "problem" << std::endl;
				}
			}
		}
	}

	void SendShipping() {
		static constexpr size_t kShipsBitsCount = 100;
		std::string login_and_ships_bits = login + ":";
		login_and_ships_bits.reserve(kShipsBitsCount + login_and_ships_bits.size());
		for (int y_coord = 0; y_coord != 10; ++y_coord) {
			for (int x_coord; x_coord != 10; ++x_coord) {
				login_and_ships_bits.push_back(ships_[y_coord][x_coord] == WATER ? 0 : 1);
			}
		}
		std::stringstream stream;
		stream << "POST" << kHttpLineEnd << "Content-Length: " << login_and_ships_bits.size()
				<< kHttpHeaderMessageDelimiter << login_and_ships_bits;
		std::string query = stream.str();

		Send(query.c_str(), query.size());
	}

	bool IsStepCorrect(const Step& step) const {
		return step.first >= 0 && step.first < 10 && step.second >= 0 && step.second < 10
				&& enemy_ships_[step.first][step.second] == WATER;
	}

	Step TryToKillHalfShip() const {
		Step half_ship_piece = *current_half_ship_pieces_.begin();
		const size_t dead_piece_y = half_ship_piece.first;
		const size_t dead_piece_x = half_ship_piece.second;
		if (current_half_ship_direction == HORIZONTAL) {
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
		} else if (current_half_ship_direction == VERTICAL) {
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
		} else if (current_half_ship_direction == UNKNOWN) {
			std::vector<Step> step_candidates =
					{ { dead_piece_y - 1, dead_piece_x }, { dead_piece_y, dead_piece_x + 1 }, { dead_piece_y
							+ 1, dead_piece_x }, { dead_piece_y, dead_piece_x - 1 } };
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

	Step MakeStep() const {
		static std::mt19937 generator(838);
		if (!current_half_ship_pieces_.empty()) {
			return TryToKillHalfShip();
		}

		//  There is no half ship
		std::uniform_int_distribution<int> potential_steps_indices(0, potential_steps_.size() - 1);
		const size_t potential_step_index = potential_steps_indices(generator);
		auto potential_step_iter = potential_steps_.begin();
		for (size_t i = 0; i != potential_step_index; ++i) {
			++potential_step_iter;
		}
		return *potential_step_iter;
	}

	static std::string WrapMessage(const std::string& message) {
		std::stringstream query_stream;
		query_stream << "POST" << kHttpLineEnd << "Content-Length: " << message.size()
				<< kHttpHeaderMessageDelimiter << message;
		return query_stream.str();
	}

	void SendStep(const Step& step) const {
		std::stringstream message_stream;
		message_stream << login << ":step:" << step.first + 1 << ":" << step.second + 1;
		std::string query = WrapMessage(message_stream.str());

		Send(query.c_str(), query.size());
	}

	void SendToCheck() const {
		static const std::string kCheckMessage(login + ":");
		static const std::string kCheckQuery = WrapMessage(kCheckMessage);

		Send(kCheckQuery.c_str(), kCheckQuery.size());
	}

	std::string RecieveAnswer() const {
		static const std::string kContentLengthHeader("Content-Length: ");
		static const size_t kContentLengthHeaderLen = kContentLengthHeader.size();
		std::string answer;
		size_t size_to_recv = 0;
		for (bool content_length_found = false; !content_length_found;) {
			char buf[1000000] = "";
			recv(server_socket, buf, sizeof(buf), 0);
			answer.append(buf);
			size_t content_length_header_begpos = answer.find(kContentLengthHeader);
			size_t http_header_endpos = answer.find(kHttpHeaderMessageDelimiter);
			if (content_length_header_begpos != std::string::npos
					&& http_header_endpos != std::string::npos) {
				content_length_found = true;
				const size_t content_length_endpos = answer.find(kHttpLineEnd,
						content_length_header_begpos);
				const size_t content_length_begpos = content_length_header_begpos + kContentLengthHeaderLen;
				const size_t content_length =
						std::atoi(
								answer.substr(content_length_begpos,
										content_length_endpos - (content_length_begpos)).c_str());
				const size_t read_content_length = answer.size()
						- (http_header_endpos + kHttpHeaderMessageDelimiterSize);
				size_to_recv = content_length - read_content_length;
			}
		}

		for (; size_to_recv > 0;) {
			char buf[1000000] = "";
			int res = recv(server_socket, buf, sizeof(buf), 0);
			answer.append(buf);
			size_to_recv -= res;
		}

		return answer;
	}

//Skipping HTTP header, its end is indicated by an empty line
	static std::string SkipHeaders(const std::string& query) {
		int begposition_of_message_itself = query.find(kHttpHeaderMessageDelimiter)
				+ kHttpHeaderMessageDelimiterSize;
		return query.substr(begposition_of_message_itself);

	}

	void FindOutHalfShipDirection(const Step& half_ship_dead_piece) {
		const size_t y_coord = half_ship_dead_piece.first;
		const size_t x_coord = half_ship_dead_piece.second;
		if (current_half_ship_pieces_.find(Step(y_coord - 1, x_coord))
				!= current_half_ship_pieces_.end()
				|| current_half_ship_pieces_.find(Step(y_coord + 1, x_coord))
						!= current_half_ship_pieces_.end()) {
			current_half_ship_direction = VERTICAL;
		} else if (current_half_ship_pieces_.find(Step(y_coord, x_coord - 1))
				!= current_half_ship_pieces_.end()
				|| current_half_ship_pieces_.find(Step(y_coord, x_coord + 1))
						!= current_half_ship_pieces_.end()) {
			current_half_ship_direction = HORIZONTAL;
		} else {
			std::cout << "Problem in FindOutHalfShipDirection" << std::endl;
		}
	}

	void EraseFromPotentialSteps(const Step& useless_step) {
		auto iter = potential_steps_.find(useless_step);
		if (iter != potential_steps_.end()) {
			potential_steps_.erase(iter);
		}
	}

	void ProcessFieldTwoHalfOrMissAnswer(const std::string& message_itself) {
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
			if (current_half_ship_direction == UNKNOWN && current_half_ship_pieces_.size() == 2) {
				FindOutHalfShipDirection(Step(y_coord, x_coord));
			}
		} else if (message_itself.find("miss") != std::string::npos) {
			enemy_ships_[y_coord][x_coord] = MISSED;
			status = WAITING;
		}
	}

	void ProcessMissedAroundKilled(const std::set<Step>& killed) {
		auto left_up = killed.lower_bound(Step(0, 0));
		auto right_down = killed.upper_bound(Step(10, 10));
		for (int i = std::max(0, left_up->first - 1); i <= std::min(9, right_down->first + 1); ++i) {
			if (left_up->second - 1 >= 0) {
				enemy_ships_[i][left_up->second - 1] = MISSED;
				EraseFromPotentialSteps(Step(i, left_up->second - 1));
			}
			if (right_down->second + 1 <= 9) {
				enemy_ships_[i][right_down->second + 1] = MISSED;
				EraseFromPotentialSteps(Step(i, right_down->second + 1));
			}
		}

		for (int i = std::max(0, left_up->second - 1); i <= std::min(9, right_down->second + 1); ++i) {
			if (left_up->first - 1 >= 0) {
				enemy_ships_[left_up->first - 1][i] = MISSED;
				EraseFromPotentialSteps(Step(left_up->first - 1, i));
			}
			if (right_down->first + 1 <= 9) {
				enemy_ships_[right_down->first + 1][i] = MISSED;
				EraseFromPotentialSteps(Step(right_down->first + 1, i));
			}
		}
	}

	void ProcessFieldTwoKillAnswer(const std::string& message_itself) {
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
		current_half_ship_direction = UNKNOWN;
	}

	void ProcessAnswer(const std::string& answer) {
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
		}
	}

	void Run(const std::string &host, int port) {
		static const std::string kGetQery = "GET / HTTP" + kHttpHeaderMessageDelimiter;
		static const std::string kLoginHeader("your_login\" class=\"");
		static constexpr size_t kLoginLength = 3;
		server_socket = socket(AF_INET, SOCK_STREAM, 0);
		Connect(host, port);
		Send(kGetQery.c_str(), kGetQery.size());
		std::string html_page = RecieveAnswer();
		const size_t login_beg_pos = html_page.find(kLoginHeader) + kLoginHeader.size();
		login = html_page.substr(login_beg_pos, kLoginLength);
		while (true) {
			sleep(1);
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
			const std::string answer = RecieveAnswer();
			ProcessAnswer(answer);
		}
	}

private:
	int server_socket;
	size_t status = SHIPPING;
	std::string login;
	std::vector<std::vector<size_t>> ships_;
	std::vector<std::vector<size_t>> enemy_ships_;
	std::set<Step> potential_steps_;
	std::set<Step> current_half_ship_pieces_;
	size_t current_half_ship_direction = UNKNOWN;
}
;

int main(int argc, char** argv) {
	static const std::string host = "127.0.0.1";
  if (argc != 1) {
    std::printf("Usage: %s port_to_connect\n", argv[0]);
    return 1;
  }

  int port_to_connect = std::atoi(argv[1]);
	Bot bot;
	bot.Run(host, port_to_connect);

	return 0;
}

