#pragma once
#include <string>
#include <vector>

#include "json.hpp"

using json = nlohmann::json;

/**
 * @brief Информация о процессе (один экземпляр)
 *
 * Поля:
 * - pid: идентификатор процесса
 * - name: имя процесса (из /proc/[pid]/comm)
 * - cpu_percent: загрузка CPU в процентах (0..100)
 * - mem_percent: доля физической памяти в процентах (0..100)
 * - rss_mb: физическая память (Resident Set Size) в мегабайтах
 * - virt_mb: виртуальная память (VSZ) в мегабайтах
 * - state: состояние процесса (R, S, D, Z, T и т.д.)
 * - threads: количество потоков
 */
struct ProcessInfo {
	int pid, threads;
	std::string name;
	float cpu_percent, mem_percent;
	long rss_mb, virt_mb;
	char state; // хранится как char, при сериализации станет числом (ASCII) –
				// для простоты
};

// Автоматическая сериализация ProcessInfo в JSON (библиотека nlohmann/json)
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(ProcessInfo, pid, name, cpu_percent,
								   mem_percent, rss_mb, virt_mb, state, threads)

/**
 * @brief Системная статистика (CPU, память, uptime)
 */
struct SystemStats {
	float total_cpu_percent;   // общая загрузка CPU (0..100)
	float memory_used_percent; // процент использованной RAM
	long total_ram_mb;		   // всего RAM (МБ)
	long used_ram_mb; // используемая RAM (МБ) – вычисляется как total -
					  // available
	long free_ram_mb;	// свободная RAM (МБ) – из MemFree
	long cached_ram_mb; // кэш страниц (МБ) – из Cached
	long uptime_seconds; // время работы системы (секунды)
};
NLOHMANN_DEFINE_TYPE_NON_INTRUSIVE(SystemStats, total_cpu_percent,
								   memory_used_percent, total_ram_mb,
								   used_ram_mb, free_ram_mb, cached_ram_mb,
								   uptime_seconds)

/**
 * @brief Обновить список процессов, читая /proc
 * @param processes [out] вектор, который будет заполнен актуальными данными
 *
 * Функция читает все PID из /proc, для каждого парсит /proc/[pid]/stat,
 * вычисляет процент CPU на основе разницы с предыдущим вызовом,
 * а также заполняет остальные поля. Процент памяти вычисляется после сбора
 * всех процессов (т.к. нужно знать общий объём RAM).
 */

void refresh_process_list(std::vector<ProcessInfo> &processes);

/**
 * @brief Обновить системную статистику (общая загрузка CPU, память, uptime)
 * @param stats [out] структура с данными
 *
 * Читает /proc/meminfo для памяти, /proc/uptime для времени работы,
 * /proc/stat для общей загрузки CPU (тоже на основе разницы вызовов).
 */
void refresh_system_stats(SystemStats &stats);