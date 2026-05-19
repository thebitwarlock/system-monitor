/**
 * @file main.cpp
 * @brief Точка входа в программу: HTTP-сервер, фоновый сбор данных, REST API.
 */

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include <chrono> // std::chrono::milliseconds
#include <fstream>
#include <iostream>
#include <mutex> // std::mutex, std::lock_guard
#include <sstream>
#include <thread> // std::thread

#include "httplib.h"
#include "json.hpp"
#include "system_res.h"

using json = nlohmann::json;

// Глобальные данные, разделяемые между потоком обновления и HTTP-обработчиками.
std::vector<ProcessInfo> g_processes; // актуальный список процессов
SystemStats g_system_stats; // актуальная системная статистика
std::mutex g_data_mutex; // мьютекс для синхронизации доступа

/**
 * @brief Фоновый поток: периодически собирает данные из /proc и обновляет
 * глобальные переменные.
 *
 * Запускается один раз при старте программы и работает бесконечно.
 * Интервал обновления – 2 секунды.
 */
void background_updater() {
	while (true) {
		std::vector<ProcessInfo> new_procs;
		SystemStats new_stats;

		refresh_process_list(new_procs);
		refresh_system_stats(new_stats);

		{
			std::lock_guard<std::mutex> lock(g_data_mutex);
			g_processes = std::move(new_procs);
			g_system_stats = new_stats;
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(2000));
	}
}

/**
 * @brief Главная функция: запускает фоновый поток и HTTP-сервер.
 *
 * Сервер слушает порт 8080 на всех интерфейсах (0.0.0.0).
 * Эндпоинты:
 *   GET /api/processes  – JSON-список процессов
 *   GET /api/system     – JSON-статистика системы
 *   GET /               – статический WebUI (из папки frontend)
 */
int main() {
	// Запускаем фоновый поток обновления данных
	std::thread updater(background_updater);
	updater.detach();

	httplib::Server svr;

	// Эндпоинт: получение списка процессов
	svr.Get("/api/processes",
			[](const httplib::Request &, httplib::Response &res) {
				std::lock_guard<std::mutex> lock(g_data_mutex);
				json j = g_processes;
				res.set_content(j.dump(), "application/json");
			});

	// Эндпоинт: получение системной статистики
	svr.Get("/api/system",
			[](const httplib::Request &, httplib::Response &res) {
				std::lock_guard<std::mutex> lock(g_data_mutex);
				json j = g_system_stats;
				res.set_content(j.dump(), "application/json");
			});

	// Раздача статических файлов (HTML, CSS, JS) из папки frontend
	// При запросе "/" будет отдан index.html
	svr.set_mount_point("/", "frontend");

	// Запуск сервера (блокирует выполнение)
	std::cout << "Server started at http://localhost:8080" << std::endl;
	svr.listen("0.0.0.0", 8080);

	return 0;
}