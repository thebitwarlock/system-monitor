/**
 * @file Скрипт для System Monitor WebUI
 * Запрашивает данные с бэкенда, отображает таблицу процессов,
 * реализует сортировку кликом по заголовку и подсветку «тяжёлых» процессов.
 */

// === Глобальные переменные ===
let currentSortColumn = '';          // имя столбца, по которому сейчас сортируем
let currentSortDirection = 'asc';    // направление: 'asc' (по возрастанию) или 'desc'
let currentProcesses = [];            // последние полученные данные о процессах

// Поля, которые должны отображаться как целые числа (без дробной части)
const integerFields = ['pid', 'threads', 'rss_mb', 'virt_mb'];

/**
 * Экранирует специальные символы HTML, чтобы предотвратить XSS и
 * корректно отображать имена вроде "init-systemd(Ub".
 * @param {string} str - входная строка
 * @returns {string} безопасная строка с заменёнными &, <, >
 */
function escapeHtml(str) {
    return String(str).replace(/[&<>]/g, function(m) {
        if (m === '&') return '&amp;';
        if (m === '<') return '&lt;';
        if (m === '>') return '&gt;';
        return m;
    });
}

/**
 * Запрашивает /api/processes, обновляет currentProcesses и перерисовывает таблицу.
 * В случае ошибки показывает сообщение в таблице.
 */
async function fetchProcesses() {
    try {
        const response = await fetch('/api/processes');
        if (!response.ok) throw new Error('HTTP error ' + response.status);
        const processes = await response.json();
        currentProcesses = processes;
        applySortAndRender();
    } catch (err) {
        console.error('Error fetching processes:', err);
        document.getElementById('table-body').innerHTML = '<tr><td colspan="10">Failed to load processes</td></tr>';
    }
}

/**
 * Запрашивает /api/system и отображает системную статистику.
 */
async function fetchSystem() {
    try {
        const response = await fetch('/api/system');
        if (!response.ok) throw new Error('HTTP error ' + response.status);
        const stats = await response.json();
        renderSystem(stats);
    } catch (err) {
        console.error('Error fetching system stats:', err);
        document.getElementById('system-stats').innerHTML = 'Failed to load system stats';
    }
}

/**
 * Применяет сортировку к currentProcesses (создаёт копию) и вызывает renderTable.
 */
function applySortAndRender() {
    if (!currentProcesses.length) {
        renderTable([]);
        return;
    }
    let sorted = [...currentProcesses];
    if (currentSortColumn) {
        sorted.sort((a, b) => {
            let valA = a[currentSortColumn];
            let valB = b[currentSortColumn];
            const isNumA = typeof valA === 'number';
            const isNumB = typeof valB === 'number';

            if (isNumA && isNumB) {
                return currentSortDirection === 'asc' ? valA - valB : valB - valA;
            }

            valA = String(valA).toLowerCase();
            valB = String(valB).toLowerCase();
            return currentSortDirection === 'asc' ? valA.localeCompare(valB) : valB.localeCompare(valA);
        });
    }
    renderTable(sorted);
}

/**
 * Отрисовывает таблицу процессов.
 * @param {Array} processes - массив объектов с данными (поля соответствуют заголовкам)
 * 
 * Логика:
 * - Заголовки строятся из ключей первого объекта.
 * - Для каждой строки определяется класс подсветки (high-cpu, high-mem).
 * - Поле `state` преобразуется из числа (ASCII) в символ.
 * - Числовые поля форматируются: целые (из integerFields) – без .00, проценты – с двумя знаками.
 * - Все данные проходят через escapeHtml.
 */
function renderTable(processes) {
    const thead = document.getElementById('table-header');
    const tbody = document.getElementById('table-body');
    if (!thead || !tbody) return;

    if (!processes || processes.length === 0) {
        tbody.innerHTML = '<tr><td colspan="10">No process data</td></tr>';
        return;
    }

    const headers = Object.keys(processes[0]);

    thead.innerHTML = `<tr>${headers.map(h => `<th data-column="${h}" style="cursor:pointer;">${h}</th>`).join('')}</tr>`;


    let htmlRows = '';
    for (let p of processes) {

        let rowClass = '';
        const cpu = p.cpu_percent || 0;
        const mem = p.mem_percent || 0;
        if (cpu > 50 && mem > 10) rowClass = 'high-cpu high-mem';
        else if (cpu > 50) rowClass = 'high-cpu';
        else if (mem > 10) rowClass = 'high-mem';
        
        let cells = '';
        for (let h of headers) {
            let val = p[h];

            if (h === 'state' && typeof val === 'number') {
                val = String.fromCharCode(val);
            } 

            else if (typeof val === 'number') {
                if (integerFields.includes(h)) {
                    val = Math.round(val);
                } else {
                    val = val.toFixed(2);
                }
            } 

            else if (val === undefined || val === null) {
                val = '?';
            }
            cells += `<td>${escapeHtml(val)}</td>`;
        }
        htmlRows += `<tr${rowClass ? ` class="${rowClass}"` : ''}>${cells}</tr>`;
    }
    tbody.innerHTML = htmlRows;
}

/**
 * Отображает системную статистику (блок над таблицей).
 * @param {Object} stats - объект с полями total_cpu_percent, used_ram_mb, total_ram_mb и т.д.
 */
function renderSystem(stats) {
    const container = document.getElementById('system-stats');
    if (!container) return;
    container.innerHTML = `
        <div><strong>CPU:</strong> ${(stats.total_cpu_percent || 0).toFixed(1)}%</div>
        <div><strong>RAM:</strong> ${stats.used_ram_mb || 0} / ${stats.total_ram_mb || 0} MB (${(stats.memory_used_percent || 0).toFixed(1)}%)</div>
        <div><strong>Uptime:</strong> ${Math.floor((stats.uptime_seconds || 0) / 3600)}h ${Math.floor(((stats.uptime_seconds || 0) % 3600) / 60)}m</div>
        <div><strong>Processes:</strong> ${currentProcesses.length}</div>
    `;
}

// === Обработчик клика по заголовку таблицы (сортировка) ===
document.getElementById('proc-table')?.addEventListener('click', (e) => {
    const th = e.target.closest('th');
    if (!th) return;
    const column = th.getAttribute('data-column');
    if (!column) return;

    if (currentSortColumn === column) {
        currentSortDirection = currentSortDirection === 'asc' ? 'desc' : 'asc';
    } else {
        currentSortColumn = column;
        currentSortDirection = 'asc';
    }
    applySortAndRender();
});

// === Автообновление каждые 2 секунды ===
setInterval(() => {
    fetchProcesses();
    fetchSystem();
}, 2000);

// Первоначальная загрузка данных
fetchProcesses();
fetchSystem();