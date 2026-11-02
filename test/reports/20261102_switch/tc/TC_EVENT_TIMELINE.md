# TC 场景 STA Send Plan 事件时间线

以下时间均来自实际 Web 快照；相对时间以各场景 netem 安装完成为 0 秒。

## T1（1%）

- `2026-11-02T09:00:06.306526+00:00` / baseline / baseline: DATA=single/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular

## T2（3%）

- `2026-11-02T09:05:57.879312+00:00` / baseline / baseline: DATA=single/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:06:14.964865+00:00` / +11.872s / loss: DATA=redundant/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:06:15.966367+00:00` / +12.874s / loss: DATA=single/cellular+wifi; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:06:24.990426+00:00` / +21.898s / loss: DATA=single/cellular+wifi; VIDEO=redundant/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:06:25.991704+00:00` / +22.899s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular

## T3（5%）

- `2026-11-02T09:11:44.851151+00:00` / baseline / baseline: DATA=single/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:11:52.354381+00:00` / +2.242s / loss: DATA=redundant/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:11:53.605880+00:00` / +3.493s / loss: DATA=single/cellular+wifi; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:11:55.621499+00:00` / +5.509s / loss: DATA=single/cellular+wifi; VIDEO=redundant/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:11:56.373184+00:00` / +6.260s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:03.673512+00:00` / +13.561s / loss: DATA=redundant/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:08.705970+00:00` / +18.593s / loss: DATA=single/wifi+cellular; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:11.722294+00:00` / +21.609s / loss: DATA=redundant/wifi+cellular; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:12.725271+00:00` / +22.612s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:13.476616+00:00` / +23.364s / loss: DATA=single/cellular+wifi; VIDEO=redundant/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:18.491760+00:00` / +28.379s / loss: DATA=single/cellular+wifi; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:20.497934+00:00` / +30.385s / loss: DATA=single/cellular+wifi; VIDEO=redundant/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:21.499064+00:00` / +31.386s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:22.751296+00:00` / +32.638s / loss: DATA=redundant/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:27.762918+00:00` / +37.650s / loss: DATA=single/wifi+cellular; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:12:39.041523+00:00` / +48.929s / loss: DATA=redundant/wifi+cellular; VIDEO=single/cellular+wifi; REALTIME=redundant/wifi+cellular
- `2026-11-02T09:12:42.111830+00:00` / +51.999s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/cellular+wifi

## T4（10%）

- `2026-11-02T09:15:03.258084+00:00` / baseline / baseline: DATA=single/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:15:09.727028+00:00` / +1.102s / loss: DATA=redundant/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:15:10.728533+00:00` / +2.104s / loss: DATA=single/cellular+wifi; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:15:12.480884+00:00` / +3.856s / loss: DATA=single/cellular+wifi; VIDEO=redundant/wifi+cellular; REALTIME=redundant/wifi+cellular
- `2026-11-02T09:15:15.496772+00:00` / +6.872s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/cellular+wifi
- `2026-11-02T09:15:28.282755+00:00` / +19.658s / loss: DATA=redundant/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/cellular+wifi
- `2026-11-02T09:15:33.295647+00:00` / +24.671s / loss: DATA=single/wifi+cellular; VIDEO=single/cellular+wifi; REALTIME=single/cellular+wifi
- `2026-11-02T09:15:36.057019+00:00` / +27.432s / loss: DATA=redundant/wifi+cellular; VIDEO=single/cellular+wifi; REALTIME=single/cellular+wifi
- `2026-11-02T09:15:37.059314+00:00` / +28.434s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/cellular+wifi

## T5（100%）

- `2026-11-02T09:18:18.616684+00:00` / baseline / baseline: DATA=single/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:18:24.918272+00:00` / +0.982s / loss: DATA=redundant/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:18:25.920131+00:00` / +1.983s / loss: DATA=single/cellular+wifi; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:18:27.422543+00:00` / +3.486s / loss: DATA=single/cellular+wifi; VIDEO=redundant/wifi+cellular; REALTIME=single/wifi+cellular
- `2026-11-02T09:18:28.424596+00:00` / +4.488s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/wifi+cellular
- `2026-11-02T09:18:29.927248+00:00` / +5.991s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=redundant/wifi+cellular
- `2026-11-02T09:18:30.928492+00:00` / +6.992s / loss: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/cellular+wifi
- `2026-11-02T09:18:34.434271+00:00` / +10.498s / loss: DATA=single/cellular+none; VIDEO=single/cellular+none; REALTIME=single/cellular+none
- `2026-11-02T09:19:25.482177+00:00` / +61.545s / recovery: DATA=redundant/cellular+wifi; VIDEO=redundant/cellular+wifi; REALTIME=redundant/cellular+wifi
- `2026-11-02T09:19:30.428057+00:00` / +66.491s / recovery: DATA=single/cellular+wifi; VIDEO=single/cellular+wifi; REALTIME=single/cellular+wifi
- `2026-11-02T09:19:46.773618+00:00` / +82.837s / recovery: DATA=redundant/cellular+wifi; VIDEO=redundant/cellular+wifi; REALTIME=redundant/cellular+wifi
- `2026-11-02T09:19:51.534930+00:00` / +87.598s / recovery: DATA=single/wifi+cellular; VIDEO=single/wifi+cellular; REALTIME=single/wifi+cellular
