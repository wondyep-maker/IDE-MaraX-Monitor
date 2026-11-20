// --- 状态与变量 ---
let currentView = 0; // 0: Boot, 1: Dashboard, 2: Timer, 3: History, 4: Network
const views = ['view-boot', 'view-dashboard', 'view-timer', 'view-history', 'view-network'];

let steamTemp = 124.0;
let hxTemp = 93.4;
let targetSteam = 124;
let targetHx = 93.5;
let steamPressureBar = 1.25;
let pumpPressureBar = 0.0;
let peakShotBar = 9.2;
let heaterOn = true;
let machineMode = 'C';
let steamBoost = false;

let scaleWeight = 0.0;
let flowRate = 0.0;
let doseWeight = 18.0;
let targetYield = 36.0;
let shotsUntilClean = 50;

let chartInstance = null;
const tempHistory = [];
const steamHistory = [];

let isExtracting = false;
let extractionStartTime = 0;
let extractionInterval = null;
let currentDuration = 0;

const historyRecords = [];

function calcSteamBar(tempC) {
    if (tempC <= 100.0) return 0.0;
    const logP = 8.14019 - (1810.94 / (244.485 + tempC));
    const pAbsBar = Math.pow(10.0, logP) * 0.00133322;
    return Math.max(0.0, Math.min(3.0, pAbsBar - 1.01325));
}

function calcPumpBar(extracting, sec) {
    if (!extracting) return 0.0;
    if (sec < 2.5) return (sec / 2.5) * 3.5;
    if (sec < 5.5) return 3.5 + ((sec - 2.5) / 3.0) * 5.7;
    return 9.2 + Math.sin(sec * 2.2) * 0.12;
}

function switchView(viewIndex) {
    views.forEach(v => document.getElementById(v).classList.remove('active'));
    document.getElementById(views[viewIndex]).classList.add('active');
    currentView = viewIndex;
}

window.onload = () => {
    setTimeout(() => {
        initChart();
        switchView(1);
        startSimulation();
    }, 2200);
};

function initChart() {
    const ctx = document.getElementById('temp-chart').getContext('2d');
    for (let i = 0; i < 30; i++) {
        tempHistory.push(93.2);
        steamHistory.push(124.0);
    }

    chartInstance = new Chart(ctx, {
        type: 'line',
        data: {
            labels: new Array(30).fill(''),
            datasets: [
                {
                    label: 'HX Temp',
                    data: tempHistory,
                    borderColor: '#32ADE6',
                    borderWidth: 2,
                    tension: 0.35,
                    pointRadius: 0
                },
                {
                    label: 'Steam Temp',
                    data: steamHistory,
                    borderColor: '#FF9F0A',
                    borderWidth: 1.5,
                    borderDash: [4, 2],
                    tension: 0.35,
                    pointRadius: 0
                }
            ]
        },
        options: {
            responsive: true,
            maintainAspectRatio: false,
            animation: false,
            scales: {
                x: { display: false },
                y: {
                    display: true,
                    min: 80,
                    max: 140,
                    grid: { color: '#222' },
                    ticks: { color: '#777', font: { size: 10 } }
                }
            },
            plugins: { legend: { display: false } }
        }
    });
}

function startSimulation() {
    setInterval(() => {
        if (steamTemp < targetSteam) {
            steamTemp += (targetSteam - steamTemp) * 0.15 + 0.1;
        } else {
            steamTemp += (Math.random() - 0.48) * 0.3;
        }
        steamPressureBar = calcSteamBar(steamTemp);

        if (!isExtracting) {
            pumpPressureBar = 0.0;
            if (hxTemp < targetHx) {
                hxTemp += (targetHx - hxTemp) * 0.12 + 0.05;
            } else {
                hxTemp += (Math.random() - 0.5) * 0.15;
            }
        }

        heaterOn = steamTemp < targetSteam - 0.3;
        steamBoost = steamTemp > 126.0 || isExtracting;

        const heaterEl = document.getElementById('heater-indicator');
        if (heaterOn) heaterEl.classList.add('heater-on');
        else heaterEl.classList.remove('heater-on');

        const boostBadge = document.getElementById('boost-badge');
        boostBadge.style.display = steamBoost ? 'inline-block' : 'none';

        const readyBadge = document.getElementById('ready-badge');
        if (hxTemp >= 91.0 && hxTemp <= 95.8) {
            readyBadge.innerText = 'READY';
            readyBadge.style.background = '#30d158';
        } else if (hxTemp < 91.0) {
            readyBadge.innerText = 'WARMUP';
            readyBadge.style.background = '#ffd60a';
        } else {
            readyBadge.innerText = 'FLUSH!';
            readyBadge.style.background = '#ff453a';
        }

        if (currentView === 1) {
            document.getElementById('steam-temp').innerText = steamTemp.toFixed(1);
            document.getElementById('steam-bar').innerText = steamPressureBar.toFixed(2);
            document.getElementById('hx-temp').innerText = hxTemp.toFixed(1);
            document.getElementById('dash-pump-bar').innerText = pumpPressureBar.toFixed(1);
        }

        tempHistory.push(hxTemp);
        steamHistory.push(steamTemp);
        if (tempHistory.length > 30) {
            tempHistory.shift();
            steamHistory.shift();
        }
        if (chartInstance && currentView === 1) {
            chartInstance.update();
        }
    }, 1000);
}

function startExtraction() {
    if (isExtracting || currentView === 0) return;

    isExtracting = true;
    scaleWeight = 0.0;
    flowRate = 0.0;
    pumpPressureBar = 0.5;
    peakShotBar = 0.0;
    switchView(2);

    extractionStartTime = Date.now();
    currentDuration = 0;
    document.getElementById('shot-timer').innerHTML = '00.0<small>s</small>';
    document.getElementById('shot-weight').innerText = '0.0';
    document.getElementById('timer-bar').style.width = '0%';
    document.getElementById('timer-target-w').innerText = targetYield.toFixed(1);

    extractionInterval = setInterval(() => {
        currentDuration = (Date.now() - extractionStartTime) / 1000;
        document.getElementById('shot-timer').innerHTML =
            currentDuration.toFixed(1).padStart(4, '0') + '<small>s</small>';

        pumpPressureBar = calcPumpBar(true, currentDuration);
        if (pumpPressureBar > peakShotBar) peakShotBar = pumpPressureBar;

        const phaseEl = document.getElementById('brew-phase');
        if (pumpPressureBar < 8.5) {
            phaseEl.innerText = '📈 E61 Mechanical Ramp (振动泵建压)';
            flowRate = pumpPressureBar > 5.0 ? 0.3 : 0.0;
        } else {
            flowRate = 1.65 + Math.sin(currentDuration * 0.4) * 0.22;
            if (scaleWeight >= targetYield - 1.0) {
                phaseEl.innerText = '🎯 TARGET REACHED! 压下 E61 手柄';
            } else {
                phaseEl.innerText = '☕ OPV 9.2 bar 恒压萃取中';
            }
        }
        scaleWeight += flowRate * 0.1;
        document.getElementById('pump-bar-live').innerText = pumpPressureBar.toFixed(1);
        document.getElementById('flow-rate').innerText = flowRate.toFixed(1);
        document.getElementById('shot-weight').innerText = scaleWeight.toFixed(1);

        hxTemp -= 0.012;
        document.getElementById('timer-hx').innerText = hxTemp.toFixed(1);

        let percent = (scaleWeight / targetYield) * 100;
        if (percent > 100) percent = 100;
        document.getElementById('timer-bar').style.width = percent + '%';
    }, 100);
}

function stopExtraction() {
    if (!isExtracting) return;
    isExtracting = false;
    pumpPressureBar = 0.0;
    clearInterval(extractionInterval);

    if (currentDuration >= 3.0) {
        shotsUntilClean = Math.max(0, shotsUntilClean - 1);
        document.getElementById('maint-tag').innerText = `Clean in: ${shotsUntilClean} shots`;
        historyRecords.unshift({
            time: new Date().toLocaleTimeString('en-US', { hour12: false }),
            duration: currentDuration.toFixed(1),
            weight: scaleWeight.toFixed(1),
            bar: Math.max(9.0, peakShotBar).toFixed(1),
            temp: hxTemp.toFixed(1)
        });
        updateHistoryUI();
    }

    setTimeout(() => {
        if (currentView === 2) switchView(1);
    }, 2800);
}

function updateHistoryUI() {
    const list = document.getElementById('history-list');
    list.innerHTML = '';

    if (historyRecords.length === 0) {
        list.innerHTML = '<div class="history-item empty-state">No shots recorded yet.</div>';
        return;
    }

    historyRecords.forEach(record => {
        const item = document.createElement('div');
        item.className = 'history-item';
        item.innerHTML = `
            <div class="hist-time">${record.time}</div>
            <div class="hist-duration">${record.duration}s / ${record.weight}g (${record.bar} bar)</div>
            <div class="hist-temp">${record.temp}°C</div>
        `;
        list.appendChild(item);
    });
}

document.getElementById('btn1').addEventListener('click', () => {
    if (currentView === 0 || currentView === 2) return;
    if (currentView === 1) switchView(3);
    else if (currentView === 3) switchView(4);
    else switchView(1);
});

let btn2Timer = null;
let btn2LongFired = false;
const btn2 = document.getElementById('btn2');

btn2.addEventListener('mousedown', () => {
    if (currentView === 0) return;
    btn2LongFired = false;
    btn2Timer = setTimeout(() => {
        btn2LongFired = true;
        startExtraction();
    }, 500);
});

btn2.addEventListener('mouseup', () => {
    clearTimeout(btn2Timer);
    if (btn2LongFired) {
        if (isExtracting) stopExtraction();
    } else {
        scaleWeight = 0.0;
        machineMode = machineMode === 'C' ? 'V' : 'C';
        document.getElementById('mode-text').innerText =
            machineMode === 'C' ? 'MaraX Coffee (C)' : 'MaraX Steam (V)';
        targetSteam = machineMode === 'C' ? 124 : 138;
        document.getElementById('steam-target').innerText = targetSteam;
    }
});

document.getElementById('btn-pump').addEventListener('click', () => {
    if (isExtracting) stopExtraction();
    else startExtraction();
});

document.getElementById('btn-ratio').addEventListener('click', () => {
    const yields = [32, 36, 40, 45];
    const idx = (yields.indexOf(targetYield) + 1) % yields.length;
    targetYield = yields[idx];
    document.getElementById('target-w-btn').innerText = targetYield;
    document.getElementById('dash-ratio').innerText = '1:' + (targetYield / doseWeight).toFixed(1);
});

document.getElementById('btn-reset-clean').addEventListener('click', () => {
    shotsUntilClean = 50;
    document.getElementById('maint-tag').innerText = `Clean in: ${shotsUntilClean} shots`;
});
