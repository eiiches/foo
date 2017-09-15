#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <algorithm>
#include <thread>
#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <set>
#include <vector>
#include <stdexcept>
#include <unistd.h>
#include <fcntl.h>

struct pmc_event_type_t
{
	int event;
	int umask;
public:
	pmc_event_type_t(int event, int umask)
		: event(event), umask(umask) {}
};

template<class T, size_t N>
constexpr size_t length_of(T(&)[N]) {
	return N;
}

static const pmc_event_type_t UOPS_DISPATCHED_PORT[] = {
	pmc_event_type_t(0xa1, 0x01),
	pmc_event_type_t(0xa1, 0x02),
	pmc_event_type_t(0xa1, 0x0c),
	pmc_event_type_t(0xa1, 0x30),
	pmc_event_type_t(0xa1, 0x40),
	pmc_event_type_t(0xa1, 0x80),
};

typedef size_t msr_addr_t;

static const msr_addr_t IA32_PMC[] = {
	0xc1,
	0xc2,
	0xc3,
	0xc4,
	0xc5,
	0xc6,
	0xc7,
	0xc8,
};

static const msr_addr_t IA32_PERFEVTSEL[] = {
	0x186,
	0x187,
	0x188,
	0x189,
	0x18a,
	0x18b,
	0x18c,
	0x18d,
};

struct pmc_config_t {
	u_int8_t event_select      :8;
	u_int8_t unit_mask         :8;
	bool user_mode             :1;
	bool operating_system_mode :1;
	bool edge_detect           :1;
	bool pin_control           :1;
	bool interrupt_enable      :1;
	bool any_thread            :1;
	bool enable_counters       :1;
	bool invert_counter_mask   :1;
	u_int8_t counter_mask      :8;
	u_int64_t __reserved       :32;
} __attribute__((packed));
static_assert(sizeof(pmc_config_t) == 8, "foo");

struct msr_t {
private:
	int fd;

public:
	msr_t(const msr_t &) = delete;
	msr_t(msr_t &&rhs) {
		this->fd = rhs.fd;
		rhs.fd = -1;
	}
	msr_t &operator=(const msr_t &) = delete;
	msr_t &operator=(msr_t &&rhs) {
		if (this->fd >= 0)
			close(fd);
		this->fd = rhs.fd;
		rhs.fd = -1;
		return *this;
	}
	msr_t(const std::string &path) {
		fd = open(path.c_str(), O_RDWR);
	}
	~msr_t() {
		if (fd >= 0)
			close(fd);
	}

public:
	void wrmsr(off_t reg, u_int64_t val) {
		if (pwrite(fd, &val, 8, reg) != 8)
			throw std::runtime_error("failed write");
	}

public:
	u_int64_t rdmsr(off_t reg) const {
		u_int64_t val;
		if (pread(fd, &val, 8, reg) != 8)
			throw std::runtime_error("failed read");
		return val;
	}
};

typedef int core_id_t;
typedef int cpu_id_t;

struct cpu_t {
	cpu_id_t id;
	std::string vendor_id;
	int cpu_family;
	int model;
	std::string model_name;
	int stepping;
	int micro_code;
	float cpu_mhz;
	size_t cache_size;
	int physical_id;
	std::set<int> siblings;
	core_id_t core_id;
	int cpu_cores;
	int apicid;
	int initial_apicid;
	bool fpu;
	bool fpu_exception;
	int cpuid_level;
	bool wp;
	std::set<std::string> flags;
	float bogomips;
	size_t clflush_size;
	size_t cache_alignment;
	std::string address_sizes;
	std::string power_management;
public:
	msr_t open_msr() const {
		return msr_t("/dev/cpu/" + std::to_string(id) + "/msr");
	}
};

std::vector<std::string> split(const std::string &s, char delim) {
	std::stringstream ss(s);
	std::string token;
	std::vector<std::string> tokens;
	while (std::getline(ss, token, delim)) {
		tokens.emplace_back(token);
	}
	return tokens;
}

std::string trim(const std::string &s) {
	size_t start = 0;
	for (; start < s.length() && (s[start] == ' ' || s[start] == '\t'); ++start);
	int end = s.length() - 1;
	for (; end >= 0 && (s[end] == ' ' || s[end] == '\t'); --end);
	return s.substr(start, end - start + 1);
}

std::vector<cpu_t> cpuinfo() {
	std::ifstream cpuinfo("/proc/cpuinfo");

	std::vector<cpu_t> processors;
	cpu_t processor;

	std::string line;
	while (std::getline(cpuinfo, line)) {
		if (line.empty()) {
			processors.push_back(processor);
			processor = cpu_t();
			continue;
		}

		const auto idx = line.find(":");
		if (idx == std::string::npos)
			throw std::runtime_error("can't parse /proc/cpuinfo");

		std::string key = trim(line.substr(0, idx));
		std::string value = trim(line.substr(idx + 1));

		if (key == "core id") {
			processor.core_id = std::stoi(value);
		} else if (key == "processor") {
			processor.id = std::stoi(value);
		} else if (key == "cpu family") {
			processor.cpu_family = std::stoi(value);
		} else if (key == "model") {
			processor.model = std::stoi(value);
		} else if (key == "flags") {
			for (const auto &flag : split(value, ' '))
				processor.flags.insert(flag);
		}
		// TODO: parse more attributes
	}

	return processors;
}

struct pmc_info_t {
	int version_id;
	int num_pmc_per_thread;
	int pmc_bitwidth;
};

pmc_info_t pmcinfo() {
	u_int64_t rax;
	asm volatile (
		"cpuid"
		: "=a"(rax)
		: "a"(0x0a)
		: "ebx", "ecx", "edx"
	);
	pmc_info_t info;
	info.version_id = rax & 0xff;
	info.num_pmc_per_thread = (rax >> 8) & 0xff;
	info.pmc_bitwidth = (rax >> 16) & 0xff;
	return info;
}

int cpu_family() {
	u_int64_t rax;
	asm volatile (
		"cpuid"
		: "=a"(rax)
		: "a"(0x01)
		: "ebx", "ecx", "edx"
	);
	int family_id = (rax >> 8) & 0x0f;
	int extended_family_id = (rax >> 20) & 0xff;
	if (family_id != 0x0f) {
		return family_id;
	} else {
		return extended_family_id + family_id;
	}
}

int cpu_model() {
	u_int64_t rax;
	asm volatile (
		"cpuid"
		: "=a"(rax)
		: "a"(0x01)
		: "ebx", "ecx", "edx"
	);
	int family_id = (rax >> 8) & 0x0f;
	int model_id = (rax >> 4) & 0x0f;
	int extended_model_id = (rax >> 16) & 0x0f;
	if (family_id == 0x06 || family_id == 0x0f) {
		return (extended_model_id << 4) + model_id;
	} else {
		return model_id;
	}
}

u_int64_t rdtsc() {
	u_int32_t eax, edx;
	asm volatile (
		"rdtsc"
		: "=a"(eax), "=d"(edx)
	);
	return ((u_int64_t) edx) << 32 | eax;
}

int
main(int argc, char **argv)
{
	std::cerr << "CPU Family: " << cpu_family() << std::endl;
	std::cerr << "CPU Model: " << cpu_model() << std::endl;
	std::cerr << std::endl;

	pmc_info_t info = pmcinfo();
	std::cerr << "Version ID of architectural performance monitoring (CPUID.0AH:EAX[7:0]): " << info.version_id << std::endl;
	std::cerr << "Number of general-purpose performance monitoring counter per logical processor (CPUID.0AH:EAX[15:8]): " << info.num_pmc_per_thread << std::endl;
	std::cerr << "Bit width of general-purpose performance monitoring counter: " << info.pmc_bitwidth << std::endl;
	std::cerr << std::endl;

	if (info.version_id < 3 || cpu_family() != 6 || cpu_model() != 42) {
		std::cerr << "Sorry your CPU is not supported yet: family = " << cpu_family() << ", " << "model = " << cpu_model() << std::endl;
		exit(EXIT_FAILURE);
	}

	const std::vector<cpu_t> cpus = cpuinfo();
	if (cpus[0].flags.find("constant_tsc") == cpus[0].flags.end()) {
		std::cerr << "Cannot calculate core utilization because constant_tsc is not available on your CPU." << std::endl;
		exit(EXIT_FAILURE);
	}

	const int num_cores = std::accumulate(cpus.begin(), cpus.end(), 0, [](const int &a, const cpu_t &b) { return std::max(a, b.core_id); }) + 1;
	std::vector<std::vector<msr_t>> core_msrs(num_cores);
	for (const auto &cpu : cpus)
		core_msrs[cpu.core_id].emplace_back(cpu.open_msr());

	// configure
	for (core_id_t core_id = 0; core_id < num_cores; ++core_id) {
		for (size_t i = 0; i < length_of(UOPS_DISPATCHED_PORT); ++i) {
			const int cpu_idx = i / info.num_pmc_per_thread;
			const int pmc_idx = i % info.num_pmc_per_thread;

			pmc_config_t conf;
			memset(&conf, 0, sizeof(pmc_config_t));
			const pmc_event_type_t &port = UOPS_DISPATCHED_PORT[i];
			conf.unit_mask = port.umask;
			conf.event_select = port.event;
			conf.user_mode = true;
			conf.operating_system_mode = true;
			conf.any_thread = true;
			conf.enable_counters = true;

			auto &msr = core_msrs[core_id][cpu_idx];
			msr.wrmsr(IA32_PERFEVTSEL[pmc_idx], *(u_int64_t*)&conf);
		}
	}

	// reset
	std::vector<std::vector<u_int64_t>> values(num_cores);
	for (core_id_t core_id = 0; core_id < num_cores; ++core_id) {
		for (size_t i = 0; i < length_of(UOPS_DISPATCHED_PORT); ++i) {
			const int cpu_idx = i / info.num_pmc_per_thread;
			const int pmc_idx = i % info.num_pmc_per_thread;

			auto &msr = core_msrs[core_id][cpu_idx];
			msr.wrmsr(IA32_PMC[pmc_idx], 0);
		}
		values[core_id].resize(length_of(UOPS_DISPATCHED_PORT));
	}

	u_int64_t tsc0 = rdtsc();
	while (true) {
		usleep(1000 * 1000);

		u_int64_t tsc = rdtsc();
		u_int64_t hz = tsc - tsc0;
		tsc0 = tsc;

		for (core_id_t core_id = 0; core_id < num_cores; ++core_id) {
			fprintf(stderr, "[");
			for (size_t i = 0; i < length_of(UOPS_DISPATCHED_PORT); ++i) {
				const int cpu_idx = i / info.num_pmc_per_thread;
				const int pmc_idx = i % info.num_pmc_per_thread;

				auto &msr = core_msrs[core_id][cpu_idx];
				u_int64_t value = msr.rdmsr(IA32_PMC[pmc_idx]);
				u_int64_t &value0 = values[core_id][i];
				fprintf(stderr, "%6.2f%%", (value - value0) / (double) hz * 100);
				value0 = value;
			}
			fprintf(stderr, "] ");
		}
		fprintf(stderr, "\n");
	}

	return 0;
}
