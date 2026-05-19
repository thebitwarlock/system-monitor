#include "system_res.h"

#include <dirent.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>

// Хранилище предыдущего значения CPU-времени для каждого процесса
// Ключ: PID, значение: сумма utime+stime (в тиках) при прошлом обновлении
static std::unordered_map<int, unsigned long long> prev_process_cpu_time;
static unsigned long long prev_total_cpu_time =
	0; // общее CPU-время системы при прошлом замере
static unsigned long long prev_idle_time = 0; // idle-время при прошлом замере
static bool first_run = true; // флаг первого вызова (нельзя вычислить проценты)

/**
 * @brief Прочитать весь файл в строку
 * @param path путь к файлу
 * @return содержимое файла или пустая строка при ошибке
 */
static std::string read_file(const std::string &path) {
	std::ifstream f(path);
	if (!f.is_open())
		return "";
	std::stringstream buffer;
	buffer << f.rdbuf();
	return buffer.str();
}

/**
 * @brief Разобрать строку "cpu ..." из /proc/stat
 * @param line строка, начинающаяся с "cpu "
 * @param total [out] общее время CPU
 * (user+nice+system+idle+iowait+irq+softirq+steal)
 * @param idle [out] время idle (простой CPU)
 *
 * Формат: cpu user nice system idle iowait irq softirq steal guest guest_nice
 * Нас интересуют только первые 8 полей.
 */
static void parse_total_cpu(const std::string &line, unsigned long long &total,
							unsigned long long &idle) {
	unsigned long long user, nice, system, idle_time, iowait, irq, softirq,
		steal;
	sscanf(line.c_str(), "cpu  %llu %llu %llu %llu %llu %llu %llu %llu", &user,
		   &nice, &system, &idle_time, &iowait, &irq, &softirq, &steal);
	total = user + nice + system + idle_time + iowait + irq + softirq + steal;
	idle = idle_time;
}

/**
 * @brief Парсинг /proc/[pid]/stat для получения ключевых метрик процесса
 * @param pid идентификатор процесса
 * @param utime [out] время процесса в пользовательском режиме (тики)
 * @param stime [out] время процесса в системном режиме (тики)
 * @param state [out] состояние процесса (R,S,D,...)
 * @param rss_pages [out] физическая память в страницах
 * @param virt_pages [out] виртуальная память в страницах
 * @param threads [out] количество потоков
 * @return true если успешно, false если файл не читается или формат неверен
 *
 * @note Формат /proc/[pid]/stat содержит много полей. Мы пропускаем
 * определённое количество позиций, используя временные переменные. Подробнее:
 *         - после имени процесса (")") идут: state, ppid, pgrp, session,
 * tty_nr, tpgid, flags
 *         - затем utime, stime, cutime, cstime, priority, nice, num_threads,
 * itrealvalue, starttime, vsize, rss, ...
 */
static bool parse_stat(int pid, unsigned long long &utime,
					   unsigned long long &stime, char &state, long &rss_pages,
					   long &virt_pages, int &threads) {
	std::string path = "/proc/" + std::to_string(pid) + "/stat";
	std::string content = read_file(path);
	if (content.empty())
		return false;

	size_t open_paren = content.find('(');
	size_t close_paren = content.find(')');
	if (open_paren == std::string::npos || close_paren == std::string::npos)
		return false;

	std::istringstream iss(content.substr(close_paren + 2));

	// 1. state
	iss >> state;

	// 2. пропускаем 6 полей: ppid, pgrp, session, tty_nr, tpgid, flags
	for (int i = 0; i < 6; ++i) {
		std::string tmp;
		iss >> tmp;
	}

	// 3. utime и stime
	iss >> utime >> stime;

	// 4. пропускаем cutime, cstime, priority, nice
	for (int i = 0; i < 4; ++i) {
		std::string tmp;
		iss >> tmp;
	}

	// 5. num_threads
	iss >> threads;

	// 6. пропускаем itrealvalue, starttime
	for (int i = 0; i < 2; ++i) {
		std::string tmp;
		iss >> tmp;
	}

	// 7. vsize (virt_pages) и rss (в страницах)
	iss >> virt_pages >> rss_pages;

	return true;
}

/**
 * @brief Прочитать имя процесса из /proc/[pid]/comm
 * @param pid идентификатор процесса
 * @return имя процесса (максимум 15 символов) или "?" при ошибке
 */
static std::string get_process_name(int pid) {
	std::string path = "/proc/" + std::to_string(pid) + "/comm";
	std::string name = read_file(path);
	if (!name.empty() && name.back() == '\n')
		name.pop_back();
	if (name.empty())
		return "?";
	return name;
}

/**
 * @brief Парсинг /proc/meminfo для получения информации о RAM
 * @param total_ram [out] общий объём RAM в kB
 * @param free_ram [out] свободная RAM в kB (MemFree)
 * @param available_ram [out] доступная RAM в kB (MemAvailable)
 * @param cached [out] размер кэша страниц в kB (Cached)
 */
static void parse_meminfo(long &total_ram, long &free_ram, long &available_ram,
						  long &cached) {
	std::string content = read_file("/proc/meminfo");
	if (content.empty())
		return;

	std::istringstream iss(content);
	std::string line;
	while (std::getline(iss, line)) {
		if (line.find("MemTotal:") == 0) {
			sscanf(line.c_str(), "MemTotal: %ld kB", &total_ram);
		} else if (line.find("MemFree:") == 0) {
			sscanf(line.c_str(), "MemFree: %ld kB", &free_ram);
		} else if (line.find("MemAvailable:") == 0) {
			sscanf(line.c_str(), "MemAvailable: %ld kB", &available_ram);
		} else if (line.find("Cached:") == 0) {
			sscanf(line.c_str(), "Cached: %ld kB", &cached);
		}
	}
}

/**
 * @brief Получить время работы системы из /proc/uptime
 * @return uptime в секундах
 */
static double get_uptime() {
	std::string content = read_file("/proc/uptime");
	if (content.empty())
		return 0.0;
	double uptime;
	sscanf(content.c_str(), "%lf", &uptime);
	return uptime;
}

/**
 * @brief Обновить список процессов (реализация)
 *
 * Алгоритм:
 * 1. Прочитать все PID из /proc.
 * 2. Считать общее время CPU системы из /proc/stat (для вычисления процентов).
 * 3. Для каждого PID прочитать /proc/[pid]/stat и заполнить ProcessInfo (кроме
 * mem_percent).
 * 4. На основе разницы с предыдущими значениями вычислить CPU% для каждого
 * процесса.
 * 5. Сохранить текущие времена для следующего вызова.
 * 6. Прочитать общий объём RAM и вычислить mem_percent для всех процессов.
 */
void refresh_process_list(std::vector<ProcessInfo> &processes) {
	processes.clear();
	DIR *proc_dir = opendir("/proc");
	if (!proc_dir)
		return;

	long page_size = sysconf(_SC_PAGESIZE);

	std::vector<int> pids;
	struct dirent *entry;
	while ((entry = readdir(proc_dir)) != nullptr) {
		if (entry->d_type == DT_DIR) {
			int pid = atoi(entry->d_name);
			if (pid > 0)
				pids.push_back(pid);
		}
	}
	closedir(proc_dir);

	std::string stat_content = read_file("/proc/stat");
	unsigned long long total_cpu = 0, idle_cpu = 0;
	if (!stat_content.empty()) {
		std::istringstream iss(stat_content);
		std::string line;
		while (std::getline(iss, line)) {
			if (line.find("cpu ") == 0) {
				parse_total_cpu(line, total_cpu, idle_cpu);
				break;
			}
		}
	}

	for (int pid : pids) {
		unsigned long long utime = 0, stime = 0;
		char state = '?';
		long rss_pages = 0, virt_pages = 0;
		int threads = 0;

		if (!parse_stat(pid, utime, stime, state, rss_pages, virt_pages,
						threads))
			continue;

		unsigned long long proc_total_time = utime + stime;

		ProcessInfo info;
		info.pid = pid;
		info.name = get_process_name(pid);
		info.state = state;
		info.threads = threads;

		info.virt_mb = virt_pages * page_size / (1024 * 1024);
		info.rss_mb = rss_pages * page_size / (1024 * 1024);

		float cpu_percent = 0.0f;
		auto it = prev_process_cpu_time.find(pid);
		if (!first_run && it != prev_process_cpu_time.end() &&
			prev_total_cpu_time != 0) {
			unsigned long long delta_proc = proc_total_time - it->second;
			unsigned long long delta_total = total_cpu - prev_total_cpu_time;
			if (delta_total > 0) {
				cpu_percent = (delta_proc * 100.0f) / delta_total;
				if (cpu_percent > 100.0f)
					cpu_percent = 100.0f;
			}
		}
		info.cpu_percent = cpu_percent;
		info.mem_percent = 0.0f;

		processes.push_back(info);

		prev_process_cpu_time[pid] = proc_total_time;
	}

	long total_ram_kb = 0, dummy_free = 0, dummy_avail = 0, dummy_cached = 0;
	parse_meminfo(total_ram_kb, dummy_free, dummy_avail, dummy_cached);
	long total_ram_mb = total_ram_kb / 1024;
	if (total_ram_mb > 0) {
		for (auto &p : processes) {
			p.mem_percent = 100.0f * p.rss_mb / total_ram_mb;
		}
	}

	prev_total_cpu_time = total_cpu;
	prev_idle_time = idle_cpu;
	first_run = false;
}

/**
 * @brief Обновить системную статистику (реализация)
 *
 * Алгоритм:
 * - Память: читаем /proc/meminfo, вычисляем used = total - available.
 * - Время работы: /proc/uptime.
 * - Общая загрузка CPU: на основе разницы /proc/stat между вызовами.
 */
void refresh_system_stats(SystemStats &stats) {
	long total_ram_kb = 0, free_ram_kb = 0, available_kb = 0, cached_kb = 0;
	parse_meminfo(total_ram_kb, free_ram_kb, available_kb, cached_kb);
	stats.total_ram_mb = total_ram_kb / 1024;
	stats.used_ram_mb = (total_ram_kb - available_kb) / 1024;
	stats.free_ram_mb = free_ram_kb / 1024;
	stats.cached_ram_mb = cached_kb / 1024;
	stats.memory_used_percent =
		(total_ram_kb > 0)
			? (100.0f * (total_ram_kb - available_kb) / total_ram_kb)
			: 0.0f;

	stats.uptime_seconds = (long)get_uptime();

	static unsigned long long prev_total = 0;
	static unsigned long long prev_idle = 0;
	static bool first = true;

	std::string stat_content = read_file("/proc/stat");
	unsigned long long total_cpu = 0, idle_cpu = 0;
	if (!stat_content.empty()) {
		std::istringstream iss(stat_content);
		std::string line;
		while (std::getline(iss, line)) {
			if (line.find("cpu ") == 0) {
				parse_total_cpu(line, total_cpu, idle_cpu);
				break;
			}
		}
	}

	if (!first && prev_total != 0) {
		unsigned long long delta_total = total_cpu - prev_total;
		unsigned long long delta_idle = idle_cpu - prev_idle;
		if (delta_total > 0) {
			stats.total_cpu_percent =
				100.0f * (delta_total - delta_idle) / delta_total;
			if (stats.total_cpu_percent > 100.0f)
				stats.total_cpu_percent = 100.0f;
		} else {
			stats.total_cpu_percent = 0.0f;
		}
	} else {
		stats.total_cpu_percent = 0.0f;
	}

	prev_total = total_cpu;
	prev_idle = idle_cpu;
	first = false;
}