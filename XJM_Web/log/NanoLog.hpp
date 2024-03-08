#pragma once
#ifndef NANO_LOG_HEADER_GUARD
#define NANO_LOG_HEADER_GUARD

#include <cstdint>
#include <memory>
#include <string>
#include <iosfwd>
#include <type_traits>

namespace nanolog {
	enum class LogLevel : uint8_t { INFO, WARN, CRIT };

	class NanoLogLine {
	public:
		NanoLogLine(LogLevel level, char const* file, char const* function, uint32_t line);
		~NanoLogLine();

		NanoLogLine(NanoLogLine&&) = default;
		NanoLogLine& operator=(NanoLogLine&&) = default;

		void stringify(std::ostream& os);

		NanoLogLine& operator<<(char arg);
		NanoLogLine& operator<<(uint32_t arg);
		NanoLogLine& operator<<(int32_t arg);
		NanoLogLine& operator<<(uint64_t arg);
		NanoLogLine& operator<<(int64_t arg);
		NanoLogLine& operator<<(double arg);
		NanoLogLine& operator<<(std::string const& arg);

		template <size_t N>
		NanoLogLine& operator<<(const char(&arg)[N]) {
			encode(string_literal_t(arg));
			return *this;
		}

		template <typename Arg>
		typename std::enable_if<std::is_same<Arg, char const* >::value, NanoLogLine&>::type//只有当 Arg 的类型是 char const *（即 C 风格字符串）时，才使这个函数在编译时有效。
		operator<<(Arg const& arg) {
			encode(arg);
			return *this;
		}

		template <typename Arg>
		typename std::enable_if<std::is_same<Arg, char* >::value, NanoLogLine& >::type
		operator<<(Arg const& arg) {
			encode(arg);
			return *this;
		}

		//文件名或函数
		struct string_literal_t {
			explicit string_literal_t(char const* s) : m_s(s) {}
			char const* m_s;
		};

	private:
		char* buffer();

		template < typename Arg >
		void encode(Arg arg);

		template < typename Arg >
		void encode(Arg arg, uint8_t type_id);

		void encode(char* ananorg);
		void encode(char const* arg);
		void encode(string_literal_t arg);
		void encode_c_string(char const* arg, size_t length);
		void resize_buffer_if_needed(size_t additional_bytes);
		void stringify(std::ostream& os, char* start, char const* const end);

	private:
		size_t m_bytes_used;
		size_t m_buffer_size;
		std::unique_ptr<char[]> m_heap_buffer;
		//对于在类的构造函数或成员变量中使用固定大小数组的情况，不需要在析构函数中手动释放内存，因为它们的内存管理由对象的创建和销毁负责。
		//使用了new动态分配才需要手动释放
		char m_stack_buffer[256 - 2 * sizeof(size_t) - sizeof(decltype(m_heap_buffer)) - 8];
	};

	struct NanoLog {
		bool operator==(NanoLogLine&);//加入一条日志
		/*
		* 理想情况下应该是操作符+=
		* 无法编译，所以就这样了...
		*/
	};

	void set_log_level(LogLevel level); /*{
		loglevel.store(static_cast<unsigned int>(level), std::memory_order_release);
	}*/

	//判断该日志的日志级别是否大于等于当前系统设置的日志级别，大于等于才记录
	bool is_logged(LogLevel level); /*{
		return static_cast<unsigned int>(level) >= loglevel.load(std::memory_order_relaxed);
	}*/

	/*
     * 非保证日志记录。使用环形缓冲区来保存日志行。
     * 当环形缓冲区满时，槽中的前一条日志行将被删除。
     * 即使环形缓冲区已满，也不会阻塞生产者。
     * ring_buffer_size_mb - 日志行被推入一个 mpsc 环形缓冲区，其大小由该参数决定。
     * 由该参数决定。由于每条日志行的长度为 256 字节、 
     * ring_buffer_size = ring_buffer_size_mb * 1024 * 1024 / 256
     */

	struct NonGuaranteedLogger {
		NonGuaranteedLogger(uint32_t ring_buffer_size_mb_) : ring_buffer_size_mb(ring_buffer_size_mb_) {}
		uint32_t ring_buffer_size_mb;
	};
	//保证日志行不会被丢弃。
	struct GuaranteedLogger{
	};

	/*
     * 确保在任何日志语句之前调用 initialize()。
     * log_directory - 创建日志的位置。例如 - "/tmp/"
     * log_file_name - 文件名的根。例如 - "nanolog
     * 这将创建以下形式的日志文件
     /tmp/nanolog.1.txt
     /tmp/nanolog.2.txt
     * 等等。
     * log_file_roll_size_mb - 兆字节，之后我们滚动到下一个日志文件。
     */

	void initialize(GuaranteedLogger gl, std::string const& log_directory, std::string const& log_file_name, uint32_t log_file_roll_size_mb);

	void initialize(NonGuaranteedLogger ngl, std::string const& log_directory, std::string const& log_file_name, uint32_t log_file_roll_size_mb);
}

//加入一条日志
#define NANO_LOG(LEVEL) nanolog::NanoLog() == nanolog::NanoLogLine(LEVEL, __FILE__, __func__, __LINE__)
//判断该日志级别是否大于等于系统设置日志级别，大于等于则写入缓冲区
#define LOG_INFO nanolog::is_logged(nanolog::LogLevel::INFO) && NANO_LOG(nanolog::LogLevel::INFO)
#define LOG_WARN nanolog::is_logged(nanolog::LogLevel::WARN) && NANO_LOG(nanolog::LogLevel::WARN)
#define LOG_CRIT nanolog::is_logged(nanolog::LogLevel::CRIT) && NANO_LOG(nanolog::LogLevel::CRIT)

#endif
//https://chat.openai.com/share/0d845a4a-d3ed-4a94-9b48-994ce808c5d4