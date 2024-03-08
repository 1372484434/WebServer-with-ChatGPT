#include"NanoLog.hpp"
#include <cstring>
#include <chrono>
#include <ctime>
#include <thread>
#include <tuple>
#include <atomic>
#include <queue>
#include <fstream>
#include<time.h>
#include<bits/stdc++.h>

namespace {
	//获取当前时间的微秒级时间戳
	uint64_t timestamp_now() {
		return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::high_resolution_clock::now().time_since_epoch()).count();
	}

	/* I want [2016-10-13 00:01:23.528514] */
	void format_timestamp(std::ostream& os, uint64_t timestamp) {
		std::time_t time_t = timestamp / 1000000;
		auto gmtime = std::gmtime(&time_t);//将time_t的变量转换为一个struct tm结构体
		char buffer[32];
		strftime(buffer, 32, "&Y-%m-%d %T.", gmtime);//年月日+小时分钟秒
		char microseconds[7];
		sprintf(microseconds, "%06llu", timestamp % 1000000);
		os << '[' << buffer << microseconds << ']';
		// std::time_t time_t = timestamp / 1000000;
		// struct tm gmtime;
		// gmtime_s(&gmtime, &time_t);
		// char buffer[32];
		// strftime(buffer, 32, "&Y-%m-%d %T.", &gmtime);//年月日+小时分钟秒
		// char microseconds[7];
		// sprintf_s(microseconds, "%06llu", timestamp % 1000000);
		// os << '[' << buffer << microseconds << ']';
	}

	std::thread::id this_thread_id() {
		//thread_local 关键字的作用（只能用于全局变量或者静态变量，不能用于普通局部变量）：每个线程都会有特有的副本，每个线程独立访问和修改这些数据，线程间互不干扰。
		//其初始化只会在第一次访问时该变量时初始化，且只会进行一次。C++11标准引入的关键字。
		static thread_local const std::thread::id id = std::this_thread::get_id();
		return id;
	}

	//一个模板类，同来查找类型T在tuple类型中的位置
	template <typename T, typename Tuple>
	struct TupleIndex;

	template <typename T, typename ... Types>
	struct TupleIndex <T, std::tuple<T, Types...>> {
		static constexpr const std::size_t value = 0;
	};

	template <typename T,typename U ,typename ... Types>
	struct TupleIndex <T, std::tuple<U, Types...>> {
		static constexpr const std::size_t value = 1 + TupleIndex < T, std::tuple < Types... > >::value;
	};
}

namespace nanolog {
	typedef std::tuple<char, uint32_t, uint64_t, int32_t, int64_t, double, NanoLogLine::string_literal_t, char*> SupportedTypes;

	char const* to_string(LogLevel loglevel) {
		switch (loglevel) {
		case LogLevel::INFO:
			return "INFO";
		case LogLevel::WARN:
			return "WARN";
		case LogLevel::CRIT:
			return "CRIT";
		}
		return "XXXX";
	}

	//encode将数据写入缓冲区
	/*
	它首先使用reinterpret_cast将buffer()返回的指针转换为Arg类型的指针。
	然后，它使用赋值操作符将arg的值存储到该位置。这实质上是将arg的二进制数据写入缓冲区。
	*/
	template<typename Arg>
	void NanoLogLine::encode(Arg arg) {
		*reinterpret_cast<Arg*>(buffer()) = arg;
		m_bytes_used += sizeof(Arg);
	}

	template<typename Arg>
	void NanoLogLine::encode(Arg arg, uint8_t type_id) {
		resize_buffer_if_needed(sizeof(Arg) + sizeof(uint8_t));
		encode<uint8_t>(type_id);
		encode<Arg>(arg);
	}

	NanoLogLine::NanoLogLine(LogLevel level, char const* file, char const* function, uint32_t line) 
		: m_bytes_used(0), m_buffer_size(sizeof(m_stack_buffer))
	{
		encode<uint64_t>(timestamp_now());
		encode<std::thread::id>(this_thread_id());
		encode<string_literal_t>(string_literal_t(file));
		encode<string_literal_t>(string_literal_t(function));
		encode<uint32_t>(line);
		encode<LogLevel>(level);
	}

	NanoLogLine::~NanoLogLine() = default;

	//将一条日志缓冲区的数据读取解析出来，并写入输出流
	void NanoLogLine::stringify(std::ostream& os) {
		char* b = !m_heap_buffer ? m_stack_buffer : m_heap_buffer.get();
		char const* const end = b + m_bytes_used;
		uint64_t timestamp = *reinterpret_cast<uint64_t*>(b);
		b += sizeof(uint64_t);
		std::thread::id threadid = *reinterpret_cast<std::thread::id*>(b);
		b += sizeof(std::thread::id);
		string_literal_t file = *reinterpret_cast<string_literal_t*>(b);
		b += sizeof(string_literal_t);
		string_literal_t function = *reinterpret_cast<string_literal_t*>(b);
		b += sizeof(string_literal_t);
		uint32_t line = *reinterpret_cast<uint32_t*>(b);
		b += sizeof(uint32_t);
		LogLevel loglevel = *reinterpret_cast<LogLevel*>(b);
		b += sizeof(LogLevel);

		format_timestamp(os, timestamp);
		os << '[' << to_string(loglevel) << ']'
			<< '[' << threadid << ']'
			<< '[' << file.m_s << ':' << function.m_s << ':' << line << ']';

		stringify(os, b, end);
		os << std::endl;
		if (loglevel >= LogLevel::CRIT) os.flush();
	}

	//decode就是解析缓冲区的数据，将其写入os输出流
	template <typename Arg>
	char* decode(std::ostream& os, char* b, Arg* dummy) {
		Arg arg = *reinterpret_cast<Arg*>(b);
		os << arg;
		return b + sizeof(Arg);
	}

	template <>
	char* decode(std::ostream& os, char* b, NanoLogLine::string_literal_t* dummy) {
		NanoLogLine::string_literal_t s = *reinterpret_cast<NanoLogLine::string_literal_t*>(b);
		os << s.m_s;
		return b + sizeof(NanoLogLine::string_literal_t);
	}

	template <>
	char* decode(std::ostream& os, char* b, char** dummy) {
		while (*b != '\0') {
			os << *b;
			++b;
		}
		return ++b;//跳过字符结束符
	}

	void NanoLogLine::stringify(std::ostream& os, char* start, char const* const end) {
		if (start == end) return;

		//对指针直接解引用只会得到一个字节的数据，刚好是用一个字节存储的数据类型
		int type_id = static_cast<int>(*start);//因为输出缓冲区储存自定义的数据时，是先用1个字节存储其类型再存其实际数据。解引用得到其实际数据类型
		start++;//数据类型不写入标准输出流os中

		switch (type_id) {
		case 0:
			stringify(os, decode(os, start, static_cast<std::tuple_element<0, SupportedTypes>::type*>(nullptr)), end);
			return;
		case 1:
			stringify(os, decode(os, start, static_cast<std::tuple_element<1, SupportedTypes>::type*>(nullptr)), end);
			return;
		case 2:
			stringify(os, decode(os, start, static_cast<std::tuple_element<2, SupportedTypes>::type*>(nullptr)), end);
			return;
		case 3:
			stringify(os, decode(os, start, static_cast<std::tuple_element<3, SupportedTypes>::type*>(nullptr)), end);
			return;
		case 4:
			stringify(os, decode(os, start, static_cast<std::tuple_element<4, SupportedTypes>::type*>(nullptr)), end);
			return;
		case 5:
			stringify(os, decode(os, start, static_cast<std::tuple_element<5, SupportedTypes>::type*>(nullptr)), end);
			return;
		case 6:
			stringify(os, decode(os, start, static_cast<std::tuple_element<6, SupportedTypes>::type*>(nullptr)), end);
			return;
		case 7:
			stringify(os, decode(os, start, static_cast<std::tuple_element<7, SupportedTypes>::type*>(nullptr)), end);
			return;
		}
	}

	////返回缓冲区写指针的位置，先写m_stack_buffer,满了则把m_stack_buffer中的数据拷贝到m_heap_buffer，后面就直接写入m_heap_buffer
	char* NanoLogLine::buffer() {
		return !m_heap_buffer ? &m_stack_buffer[m_bytes_used] : &(m_heap_buffer.get())[m_bytes_used];
	}

	//判断是否需要扩容
	void NanoLogLine::resize_buffer_if_needed(size_t additional_bytes) {
		size_t const required_size = m_bytes_used + additional_bytes;

		if (required_size <= m_buffer_size) return;

		if (!m_heap_buffer) {
			m_buffer_size = std::max(static_cast<size_t>(512), required_size);
			m_heap_buffer.reset(new char[m_buffer_size]);
			memcpy(m_heap_buffer.get(), m_stack_buffer, m_bytes_used);
			return;
		}
		else{
			m_buffer_size = std::max(static_cast<size_t>(2 * m_buffer_size), required_size);
			std::unique_ptr < char[]> new_heap_buffer(new char[m_buffer_size]);
			memcpy(new_heap_buffer.get(), m_heap_buffer.get(), m_bytes_used);
			m_heap_buffer.swap(new_heap_buffer);
		}
	}

	void NanoLogLine::encode(char const* arg) {
		if (arg != nullptr) {
			encode_c_string(arg, strlen(arg));
		}
	}

	void NanoLogLine::encode(string_literal_t arg) {
		encode<string_literal_t>(arg, TupleIndex<string_literal_t, SupportedTypes>::value);
	}

	void NanoLogLine::encode(char* arg) {
		if (arg != nullptr) {
			encode_c_string(arg, strlen(arg));
		}
	}

	//将c风格的字符串写入缓冲区
	void NanoLogLine::encode_c_string(char const* arg, size_t length) {
		if (length == 0) return;
		resize_buffer_if_needed(1 + length + 1);//使用一个字节来存储类型信息，后一个+1表示结束符
		char* b = buffer();
		auto type_id = TupleIndex<char*, SupportedTypes>::value;//因为是char类型，使用一个字节刚好可以存储其类型
		*reinterpret_cast<uint8_t*> (b++) = static_cast<uint8_t> (type_id);//因为type_id位于0~7小于255，因此一个字节可以表示，因此将type_id转换为uint8_t不会出错
		memcpy(b, arg, length + 1);
		m_bytes_used += 1 + length + 1;
	}


	NanoLogLine& NanoLogLine::operator<<(std::string const& arg) {
		encode_c_string(arg.c_str(), arg.length());
		return *this;
	}


	NanoLogLine& NanoLogLine::operator<<(int32_t arg) {
		encode<int32_t>(arg, TupleIndex<int32_t, SupportedTypes>::value);
		return *this;
	}

	NanoLogLine& NanoLogLine::operator<<(uint32_t arg) {
		encode<uint32_t>(arg, TupleIndex<uint32_t, SupportedTypes>::value);
		return *this;
	}

	NanoLogLine& NanoLogLine::operator<<(int64_t arg) {
		encode<int64_t>(arg, TupleIndex<int64_t, SupportedTypes>::value);
		return *this;
	}

	NanoLogLine& NanoLogLine::operator<<(uint64_t arg) {
		encode<uint64_t>(arg, TupleIndex<uint64_t, SupportedTypes>::value);
		return *this;
	}

	NanoLogLine& NanoLogLine::operator<<(double arg) {
		encode<double>(arg, TupleIndex<double, SupportedTypes>::value);
		return *this;
	}

	NanoLogLine& NanoLogLine::operator<<(char arg) {
		encode<char>(arg, TupleIndex<char, SupportedTypes>::value);
		return *this;
	}

	//缓存类基类
	class BufferBase {
	public:
		virtual ~BufferBase() = default;
		virtual void push(NanoLogLine&& logline) = 0;
		virtual bool try_pop(NanoLogLine& logline) = 0;
	};

	//实现自旋锁
	struct SpinLock {
		SpinLock(std::atomic_flag& flag) : m_flag(flag) {
			while (m_flag.test_and_set(std::memory_order_acquire));//通过while循环实现忙等待，直到获取锁
		}
		~SpinLock() {
			m_flag.clear(std::memory_order_release);//使用内存屏障，使得别的线程知道该锁的消息
		}
	private:
		std::atomic_flag& m_flag;//一个布尔标识
	};

	//针对非保证日志不丢失模式，使用环形缓冲区
	class RingBuffer : public BufferBase {
	public:
		//将每条日志行封装为一个完整的项，以便在环形缓冲区中进行存储和读取。
		struct alignas(64) Item {
			Item() : flag{ATOMIC_FLAG_INIT}, written(0), logline(LogLevel::INFO, nullptr, nullptr, 0)
			{
			}

			std::atomic_flag flag;
			char written;//标志位，表示该item是否存有一条日志
			//填充字节数组，用于对其item结构体到256大小字节，避免内存浪费和伪共享
			//因为通常以缓存行（cache line）作为最小的内存读写单位，缓存行的大小通常是64字节。
			//当不同的Item对象位于同一个Cache Line时，多个线程同时访问不同的Item对象会导致伪共享问题。
			char padding[256 - sizeof(std::atomic_flag) - sizeof(char) - sizeof(NanoLogLine)];
			NanoLogLine logline;
		};

		RingBuffer(size_t const size)
			: m_size(size), m_ring(static_cast<Item*>(std::malloc(size * sizeof(Item)))), m_write_index(0), m_read_index(0)
		{
			for (size_t i = 0; i < m_size; i++) {
				new(&m_ring[i]) Item();
			}
			static_assert(sizeof(Item) == 256, "Unexpected size != 256");
		}

		~RingBuffer() {
			for (size_t i = 0; i < m_size; i++) {
				m_ring[i].~Item();
			}
			std::free(m_ring);
		}

		//将一条日志写入环形缓存冲区
		void push(NanoLogLine&& logline)  override {
			//没有判断该位置是否有日志，如果有日志，说明满了，也不阻塞，直接将旧日志替换掉
			unsigned int write_index = m_write_index.fetch_add(1, std::memory_order_relaxed) % m_size;//写入一条日志
			Item& item = m_ring[write_index];
			SpinLock spinlock(item.flag);
			item.logline = std::move(logline);
			item.written = 1;
		}

		bool try_pop(NanoLogLine& logline)  override {
			Item& item = m_ring[m_read_index % m_size];
			SpinLock spinlock(item.flag);
			if (item.written == 1) {
				logline = std::move(item.logline);
				item.written = 0;
				++m_read_index;
				return true;
			}
			return false;
		}

		RingBuffer(RingBuffer const&) = delete;
		RingBuffer& operator=(RingBuffer const&) = delete;

	private:
		size_t const m_size;//保存日志的条数
		Item* m_ring;
		//使用原子变量可以保证多个线程对该变量的读写操作是原子性的，即不会被其他线程中断，从而避免了数据竞争的问题。
		std::atomic<unsigned int> m_write_index;//多生产者，每次push一条日志
		unsigned int m_read_index;//单消费者
	};

	class Buffer {
	public:
		struct Item {
			Item(NanoLogLine&& nanologline) : logline(std::move(nanologline)) {}
			char padding[256 - sizeof(NanoLogLine)];
			NanoLogLine logline;
		};

		static constexpr const size_t size = 32768;//256 * 32768 = 8MB

		Buffer() : m_buffer(static_cast<Item*>(std::malloc(size * sizeof(Item))))
		{
			for (size_t i = 0; i <= size; i++) {
				//使用了原子操作，并指定了松散的内存序，可以在多线程环境下安全地进行操作，不需要强制同步。
				m_write_state[i].store(0, std::memory_order_relaxed);
			}
			static_assert(sizeof(Item) == 256, "Unexpected size != 256");
		}

		~Buffer() {
			unsigned int write_count = m_write_state[size].load();
			for (size_t i = 0; i < write_count; i++) {
				m_buffer[i].~Item();
			}
			std::free(m_buffer);
		}

		/*
			atomic原子操作的几个函数：
				sotre()：赋值
				load()：获取当前的值
				fetch_add(a, std::memory_order_acquire)：将当前值+a且返回旧值(即+a之前的值)
		*/

		bool push(NanoLogLine&& logline, unsigned int const write_index) {
			new (&m_buffer[write_index]) Item(std::move(logline));
			m_write_state[write_index].store(1, std::memory_order_release);//赋值
			//后面+1是因为fetch_add返回的是+1前的值，因此需要+1
			return m_write_state[size].fetch_add(1, std::memory_order_acquire) + 1 == size;
		}

		bool try_pop(NanoLogLine& logline, unsigned int const read_index) {
			if (m_write_state[read_index].load(std::memory_order_acquire)) {
				Item& item = m_buffer[read_index];
				logline = std::move(item.logline);
				return true;
			}
			return false;
		}
		
		Buffer(Buffer const&) = delete;
		Buffer& operator=(Buffer const&) = delete;

	private:
		Item* m_buffer;
		//atomic保证在多线程环境下对共享变量的操作是原子的，避免了数据竞争和死锁等问题。最后+1是使用m_write_state[size]来存储buffer存储了多少条日志
		std::atomic<unsigned int> m_write_state[size + 1];
	};

	class QueueBuffer : public BufferBase {
	public:
		QueueBuffer(QueueBuffer const&) = delete;
		QueueBuffer& operator=(QueueBuffer const&) = delete;

		QueueBuffer()
			: m_current_read_buffer{ nullptr }, m_write_index(0), m_flag{ ATOMIC_FLAG_INIT }, m_read_index(0)
		{
			set_next_new_write_buffer();
		}

		void push(NanoLogLine&& logline)  override {
			unsigned int write_index = m_write_index.fetch_add(1, std::memory_order_relaxed);
			if (write_index < Buffer::size) {
				if (m_current_write_buffer.load(std::memory_order_acquire)->push(std::move(logline), write_index)) {
					set_next_new_write_buffer();
				}
			}
			else {
				while (m_write_index.load(std::memory_order_acquire) >= Buffer::size);
				push(std::move(logline));
			}
		}

		bool try_pop(NanoLogLine& logline)  override {
			if (m_current_read_buffer == nullptr) m_current_read_buffer = get_next_new_read_buffer();

			Buffer* read_buffer = m_current_read_buffer;

			if (read_buffer == nullptr) return false;

			if (bool success = read_buffer->try_pop(logline, m_read_index)) {
				m_read_index++;
				if (m_read_index == Buffer::size) {
					m_read_index = 0;
					m_current_read_buffer = nullptr;
					SpinLock spinlock(m_flag);
					m_buffers.pop();
				}
				return true;
			}
			return false;
		}
		
	private:
		//new一个新的buffer，将其加入buffer队列中
		void set_next_new_write_buffer() {
			std::unique_ptr<Buffer> next_new_write_buffer(new Buffer());
			m_current_write_buffer.store(next_new_write_buffer.get(), std::memory_order_release);
			SpinLock spinlock(m_flag);
			m_buffers.push(std::move(next_new_write_buffer));
			m_write_index.store(0, std::memory_order_relaxed);
		}

		Buffer* get_next_new_read_buffer() {
			SpinLock spinlock(m_flag);
			return m_buffers.empty() ? nullptr : m_buffers.front().get();
		}

	private:
		//多生产者单消费者，对写指针需要使用原子操作，避免多线程竞争；读指针则不需要
		std::queue<std::unique_ptr<Buffer>> m_buffers;
		std::atomic<Buffer*> m_current_write_buffer;
		Buffer* m_current_read_buffer;
		std::atomic<unsigned int> m_write_index;
		unsigned int m_read_index;
		std::atomic_flag m_flag;
	};

	class FileWriter {
	public:
		FileWriter(std::string const& log_directory, std::string const& log_file_name, uint32_t log_file_roll_size_mb)
			: m_log_file_roll_size_bytes(log_file_roll_size_mb * 1024 * 1024), m_name(log_directory + log_file_name)
		{
			roll_file();
		}

		void write(NanoLogLine& logline) {
			auto pos = m_os->tellp();//获取当前输出流的写入位置指针
			logline.stringify(*m_os);
			m_bytes_written += m_os->tellp() - pos;
			if (m_bytes_written > m_log_file_roll_size_bytes) {
				roll_file();
			}
		}

	private:
		//将输出流的二进制数据写入文件，并建立新日志文件
		void roll_file() {
			if (m_os) {
				m_os->flush();
				m_os->close();
			}

			m_bytes_written = 0;
			m_os.reset(new std::ofstream());

			std::string log_file_name = m_name;
			log_file_name.append(".");
			log_file_name.append(std::to_string(++m_file_number));
			log_file_name.append(".txt");

			m_os->open(log_file_name, std::ofstream::out | std::ofstream::trunc);
		}

	private:
		uint32_t m_file_number = 0;//表示写到第几个文件了
		std::streamoff m_bytes_written = 0;
		uint32_t const m_log_file_roll_size_bytes;//每个文件的最大字节数
		std::string const m_name;//保存日志文件的完整路径（路径+文件名）
		std::unique_ptr<std::ofstream> m_os;
	};

	class NanoLogger {
	public:
		NanoLogger(NonGuaranteedLogger ngl, std::string const& log_directory, std::string const& log_file_name, uint32_t log_file_roll_size_mb)
			: m_state(State::INIT), m_buffer_base(new RingBuffer(std::max(1u, ngl.ring_buffer_size_mb) * 1024 * 4)),
			m_file_writer(log_directory, log_file_name, std::max(1u, log_file_roll_size_mb)), m_thread(&NanoLogger::pop, this)
		{
			m_state.store(State::READY, std::memory_order_release);
		}

		NanoLogger(GuaranteedLogger gl, std::string const& log_directory, std::string const& log_file_name, uint32_t log_file_roll_size_mb)
			: m_state(State::INIT), m_buffer_base(new QueueBuffer())
			, m_file_writer(log_directory, log_file_name, log_file_roll_size_mb), m_thread(&NanoLogger::pop, this)
		{
			m_state.store(State::READY, std::memory_order_release);
		}

		~NanoLogger() {
			m_state.store(State::SHUTDOWN);
			m_thread.join();
		}

		void add(NanoLogLine&& logline) {
			m_buffer_base->push(std::move(logline));
		}

		void pop() {
			while (m_state.load(std::memory_order_acquire) == State::INIT) {
				std::this_thread::sleep_for(std::chrono::microseconds(50));
			}

			NanoLogLine logline(LogLevel::INFO, nullptr, nullptr, 0);

			while (m_state.load() == State::READY) {
				if (m_buffer_base->try_pop(logline)) {
					m_file_writer.write(logline);
				}
				else {
					std::this_thread::sleep_for(std::chrono::microseconds(50));
				}
			}

			while (m_buffer_base->try_pop(logline)) {
				m_file_writer.write(logline);//???
			}
		}

	private:
		enum class State{INIT, READY, SHUTDOWN};

		std::atomic<State> m_state;
		std::unique_ptr<BufferBase> m_buffer_base;
		FileWriter m_file_writer;
		std::thread m_thread;
	};

	std::unique_ptr<NanoLogger> nanologger;
	std::atomic<NanoLogger*> atomic_nanologger;

	//加入一条日志
	bool NanoLog::operator==(NanoLogLine& logline) {
		atomic_nanologger.load(std::memory_order_acquire)->add(std::move(logline));
		return true;
	}

	//unique_str.reset()有参就是先删除当前unique_ptr所管理的对象（如果有的话），然后将unique_ptr设置为new_pointer，从而接管新指针的所有权。
	//atomic.store() 是一个C++函数，用于设置原子对象的值，它将一个新的值存储到原子对象中。
	//C++标准库中的 std::atomic 类型的操作可以带有内存顺序参数，用来指定多线程间的同步级别。
	//std::memory_order_seq_cst：提供最强的同步级别，所有线程看到的操作顺序是一致的。
	void initialize(GuaranteedLogger gl, std::string const& log_directory, std::string const& log_file_name, uint32_t log_file_roll_size_mb)
	{
		nanologger.reset(new NanoLogger(gl, log_directory, log_file_name, log_file_roll_size_mb));
		atomic_nanologger.store(nanologger.get(), std::memory_order_seq_cst);
		
	}
	
	//另一个重载版本
	void initialize(NonGuaranteedLogger ngl, std::string const& log_directory, std::string const& log_file_name, uint32_t log_file_roll_size_mb)
	{
		nanologger.reset(new NanoLogger(ngl, log_directory, log_file_name, log_file_roll_size_mb));
		atomic_nanologger.store(nanologger.get(), std::memory_order_seq_cst);
	}

	std::atomic<unsigned int> loglevel = { 0 };

	void set_log_level(LogLevel level) {
		loglevel.store(static_cast<unsigned int>(level), std::memory_order_release);
	}

	//判断该日志的日志级别是否大于等于当前系统设置的日志级别，大于等于才记录
	bool is_logged(LogLevel level) {
		return static_cast<unsigned int>(level) >= loglevel.load(std::memory_order_relaxed);
	}
}