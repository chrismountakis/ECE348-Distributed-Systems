%% =========================================================================
%  MyNFS Cache Benchmark — Results Visualization
%  Three tests, three figures.
%
%  HOW TO RUN THE TESTS (from the project3 directory):
%
%  1. Start the server:
%       ./server rootdir 10000 >/dev/null 2>&1 &
%
%  2. Test 1 — Re-read benchmark (run twice, once per cache config):
%       tests/test_cache 127.0.0.1 10000 test_10k.bin out_p1.bin out_p2.bin 1
%       tests/test_cache 127.0.0.1 10000 test_10k.bin out_p1.bin out_p2.bin 20
%
%  3. Test 2 — Block-size sweep (run once per block size):
%       for bs in 128 256 512 1024 2048; do
%           tests/test_blocksize 127.0.0.1 10000 test_10k.bin $bs
%       done
%
%  4. Test 3 — Freshness benchmark (run twice, once per fresh_t):
%       tests/test_freshness_bench 127.0.0.1 10000 test_video.mp4 0    6000 2
%       tests/test_freshness_bench 127.0.0.1 10000 test_video.mp4 3600 6000 2
%
%  5. Stop the server:
%       kill $(pgrep -x server)
%% =========================================================================

clear; close all; clc;

%% ---- Colour palette (consistent across all figures) ---------------------
BLUE   = [0.122 0.467 0.706];
ORANGE = [0.839 0.153 0.157];
GREEN  = [0.173 0.627 0.173];

FONT_TITLE = 13;
FONT_AXIS  = 12;
FONT_TICK  = 11;
FONT_ANNOT = 10;

%% =========================================================================
%%  MEASURED DATA
%% =========================================================================

% ----- Test 1: Re-read benchmark ------------------------------------------
% File: test_video.mp4 (2 864 774 bytes), block_size=512, fresh_t=3600
% 5596 blocks (5595 x 512 B full + 1 x 134 B partial).
% Stats reset AFTER opens, so open RPCs are NOT counted.
% Each pass: 5596 read RPCs + 5596 write RPCs = 11192 total RPCs.
% Write RPCs are identical for both configs; caching only affects reads.
% Pass-2 cache bytes_recv = 5596 write acks x 36 B header = 201 456 B.
%
%   Columns: [pass1    pass2]
%   Rows:    [baseline (max_cached=1); cache (max_cached=6000)]

t1_read_rpc   = [5596 5596 ; 5596    0];          % read RPCs = cache_misses
t1_bytes_recv = [3267686 3267686 ; 3267686 201456] / 1e6;  % MB
t1_elapsed    = [266.14 251.63 ; 387.90 223.77];  % ms

% ----- Test 2: Block-size sweep -------------------------------------------
% File: test_video.mp4 (2 864 774 bytes), max_cached=100, fresh_t=3600,
% single cold pass. Each block is read exactly once so cache size does not
% affect the result.

t2_bs         = [128    256    512    1024   2048 ];
t2_rpc        = [22382  11191  5596   2798   1399 ];
t2_bytes_recv = [3670526 3267650 3066230 2965502 2915138] / 1e6;  % MB
t2_throughput = [5926.4 11700.0 24064.4 48632.5 93257.5] / 1024;  % MB/s

% ----- Test 3: Freshness-time benchmark -----------------------------------
% File: test_video.mp4 (2 864 774 bytes), block_size=512, max_cached=6000.
% fresh_t=0    -> pass-2 sends ERR_NOTMODIFIED per block (5597 x 36 B = 201 KB)
% fresh_t=3600 -> pass-2 is pure cache hits: 0 RPCs, 0 bytes, 7x faster
%
%   Columns: [pass1  pass2]
%   Rows:    [fresh_t=0; fresh_t=3600]

t3_rpc        = [5597 5597 ; 5596    0];
t3_bytes_recv = [3066266 201492 ; 3066230 0] / 1e6;   % MB
t3_elapsed    = [224.96 149.68 ; 202.46 30.34];        % ms

%% =========================================================================
%%  FIGURE 1 — Re-read Benchmark: Baseline vs Cache
%% =========================================================================
figure('Name','Test 1 — Re-read Benchmark', ...
       'NumberTitle','off', 'Position',[60 60 1200 400]);

pass_lbls = {'Pass 1  (cold cache)', 'Pass 2  (warm cache)'};
cfg_lbls  = {'Baseline  (max\_cached = 1)', 'Cache  (max\_cached = 6000)'};

% --- (a) Read RPCs ---------------------------------------------------------
subplot(1,3,1);
b = bar(t1_read_rpc', 'grouped');
b(1).FaceColor = BLUE;   b(1).DisplayName = cfg_lbls{1};
b(2).FaceColor = ORANGE; b(2).DisplayName = cfg_lbls{2};
set(gca,'XTickLabel', pass_lbls, 'FontSize', FONT_TICK);
ylabel('Read RPCs to server', 'FontSize', FONT_AXIS);
title('(a)  Server Read Round-Trips', 'FontSize', FONT_TITLE, 'FontWeight','bold');
legend('Location','northeast','FontSize', FONT_ANNOT);
ylim([0 6500]); grid on; box on;
label_bars(b, '%g', 80, FONT_ANNOT);

% --- (b) Bytes received ----------------------------------------------------
subplot(1,3,2);
b = bar(t1_bytes_recv', 'grouped');
b(1).FaceColor = BLUE;   b(1).DisplayName = cfg_lbls{1};
b(2).FaceColor = ORANGE; b(2).DisplayName = cfg_lbls{2};
set(gca,'XTickLabel', pass_lbls, 'FontSize', FONT_TICK);
ylabel('Bytes received (MB)', 'FontSize', FONT_AXIS);
title('(b)  Data Received from Server', 'FontSize', FONT_TITLE, 'FontWeight','bold');
legend('Location','northeast','FontSize', FONT_ANNOT);
grid on; box on;
% Annotate the small pass-2 cache bar (0.19 MB) so it is readable
xt = b(2).XEndPoints;
text(xt(2), 0.20 + 0.06, '0.19 MB', 'HorizontalAlignment','center', ...
    'FontSize',FONT_ANNOT,'FontWeight','bold','Color',ORANGE);

% --- (c) Elapsed time ------------------------------------------------------
subplot(1,3,3);
b = bar(t1_elapsed', 'grouped');
b(1).FaceColor = BLUE;   b(1).DisplayName = cfg_lbls{1};
b(2).FaceColor = ORANGE; b(2).DisplayName = cfg_lbls{2};
set(gca,'XTickLabel', pass_lbls, 'FontSize', FONT_TICK);
ylabel('Elapsed time (ms)', 'FontSize', FONT_AXIS);
title('(c)  Copy Pass Duration (read + write)', 'FontSize', FONT_TITLE, 'FontWeight','bold');
legend('Location','northeast','FontSize', FONT_ANNOT);
grid on; box on;
label_bars(b, '%.0f ms', 4, FONT_ANNOT);

sgtitle({'Test 1 — Re-read Benchmark', ...
         'file: test\_video.mp4  (2.8 MB)  |  block\_size = 512 B  |  fresh\_t = 3600 s'}, ...
        'FontSize',14,'FontWeight','bold');

%% =========================================================================
%%  FIGURE 2 — Block-Size Sweep
%% =========================================================================
figure('Name','Test 2 — Block-Size Sweep', ...
       'NumberTitle','off', 'Position',[60 520 1200 400]);

lw = 2.5; ms = 9;

% --- (a) RPC count ---------------------------------------------------------
subplot(1,3,1);
plot(t2_bs, t2_rpc, 'o-','Color',BLUE,'LineWidth',lw, ...
     'MarkerSize',ms,'MarkerFaceColor',BLUE);
set(gca,'XTick',t2_bs,'FontSize',FONT_TICK);
xlabel('Block size (bytes)', 'FontSize', FONT_AXIS);
ylabel('Server RPCs', 'FontSize', FONT_AXIS);
title('(a)  RPC Count vs Block Size', 'FontSize',FONT_TITLE,'FontWeight','bold');
ylim([0 25000]); grid on; box on;
for i = 1:length(t2_bs)
    text(t2_bs(i), t2_rpc(i)+700, num2str(t2_rpc(i)), ...
        'HorizontalAlignment','center','FontSize',FONT_ANNOT, ...
        'FontWeight','bold','Color',BLUE);
end

% --- (b) Bytes received ----------------------------------------------------
subplot(1,3,2);
plot(t2_bs, t2_bytes_recv, 's-','Color',ORANGE,'LineWidth',lw, ...
     'MarkerSize',ms,'MarkerFaceColor',ORANGE);
set(gca,'XTick',t2_bs,'FontSize',FONT_TICK);
xlabel('Block size (bytes)', 'FontSize', FONT_AXIS);
ylabel('Bytes received (MB)', 'FontSize', FONT_AXIS);
title('(b)  Data Received vs Block Size', 'FontSize',FONT_TITLE,'FontWeight','bold');
ylim([2.7 4.0]); grid on; box on;
for i = 1:length(t2_bs)
    text(t2_bs(i), t2_bytes_recv(i) + 0.04, ...
        sprintf('%.2f', t2_bytes_recv(i)), ...
        'HorizontalAlignment','center','FontSize',FONT_ANNOT, ...
        'FontWeight','bold','Color',ORANGE);
end

% --- (c) Throughput --------------------------------------------------------
subplot(1,3,3);
plot(t2_bs, t2_throughput, '^-','Color',GREEN,'LineWidth',lw, ...
     'MarkerSize',ms,'MarkerFaceColor',GREEN);
set(gca,'XTick',t2_bs,'FontSize',FONT_TICK);
xlabel('Block size (bytes)', 'FontSize', FONT_AXIS);
ylabel('Throughput (MB/s)', 'FontSize', FONT_AXIS);
title('(c)  Throughput vs Block Size', 'FontSize',FONT_TITLE,'FontWeight','bold');
grid on; box on;
for i = 1:length(t2_bs)
    text(t2_bs(i), t2_throughput(i)+2, sprintf('%.1f', t2_throughput(i)), ...
        'HorizontalAlignment','center','FontSize',FONT_ANNOT, ...
        'FontWeight','bold','Color',GREEN);
end

sgtitle({'Test 2 — Block-Size Sweep', ...
         'file: test\_video.mp4  (2.8 MB)  |  max\_cached = 100  |  single cold pass'}, ...
        'FontSize',14,'FontWeight','bold');

%% =========================================================================
%%  FIGURE 3 — Freshness-Time Benchmark
%% =========================================================================
figure('Name','Test 3 — Freshness Benchmark (Read Only)', ...
       'NumberTitle','off', 'Position',[60 980 1200 400]);

pass_lbls3 = {'Pass 1  (cold cache)', 'Pass 2  (warm cache)'};
fresh_lbls = {'fresh\_t = 0  (always stale)', 'fresh\_t = 3600  (long TTL)'};

% --- (a) RPCs --------------------------------------------------------------
subplot(1,3,1);
b = bar(t3_rpc', 'grouped');
b(1).FaceColor = BLUE;   b(1).DisplayName = fresh_lbls{1};
b(2).FaceColor = ORANGE; b(2).DisplayName = fresh_lbls{2};
set(gca,'XTickLabel', pass_lbls3, 'FontSize', FONT_TICK);
ylabel('Server RPCs', 'FontSize', FONT_AXIS);
title('(a)  Server Round-Trips', 'FontSize',FONT_TITLE,'FontWeight','bold');
legend('Location','northeast','FontSize',FONT_ANNOT);
grid on; box on;
xt = b(1).XEndPoints; yt = b(1).YEndPoints;
text(xt(2), yt(2)+80, '5597', 'HorizontalAlignment','center', ...
    'FontSize',FONT_ANNOT,'FontWeight','bold','Color',BLUE);
xt = b(2).XEndPoints; yt = b(2).YEndPoints;
text(xt(2), yt(2)+80, '0', 'HorizontalAlignment','center', ...
    'FontSize',FONT_ANNOT,'FontWeight','bold','Color',ORANGE);

% --- (b) Bytes received ----------------------------------------------------
subplot(1,3,2);
b = bar(t3_bytes_recv', 'grouped');
b(1).FaceColor = BLUE;   b(1).DisplayName = fresh_lbls{1};
b(2).FaceColor = ORANGE; b(2).DisplayName = fresh_lbls{2};
set(gca,'XTickLabel', pass_lbls3, 'FontSize', FONT_TICK);
ylabel('Bytes received (MB)', 'FontSize', FONT_AXIS);
title('(b)  Data Received from Server', 'FontSize',FONT_TITLE,'FontWeight','bold');
legend('Location','northeast','FontSize',FONT_ANNOT);
grid on; box on;
xt = b(1).XEndPoints;
text(xt(2), 0.24, '0.19 MB', 'HorizontalAlignment','center', ...
    'FontSize',FONT_ANNOT,'FontWeight','bold','Color',BLUE);
xt = b(2).XEndPoints;
text(xt(2), 0.10, '0 MB', 'HorizontalAlignment','center', ...
    'FontSize',FONT_ANNOT,'FontWeight','bold','Color',ORANGE);

% --- (c) Elapsed time ------------------------------------------------------
subplot(1,3,3);
b = bar(t3_elapsed', 'grouped');
b(1).FaceColor = BLUE;   b(1).DisplayName = fresh_lbls{1};
b(2).FaceColor = ORANGE; b(2).DisplayName = fresh_lbls{2};
set(gca,'XTickLabel', pass_lbls3, 'FontSize', FONT_TICK);
ylabel('Elapsed time (ms)', 'FontSize', FONT_AXIS);
title('(c)  Read Pass Duration', 'FontSize',FONT_TITLE,'FontWeight','bold');
legend('Location','northeast','FontSize',FONT_ANNOT);
grid on; box on;
label_bars(b, '%.0f ms', 2, FONT_ANNOT);

sgtitle({'Test 3 — Freshness-Time Benchmark (Read Only)', ...
         'file: test\_video.mp4  (2.8 MB)  |  block\_size = 512 B  |  max\_cached = 6000'}, ...
        'FontSize',14,'FontWeight','bold');

%% =========================================================================
%%  LOCAL FUNCTIONS  (must be at end of script file)
%% =========================================================================

function label_bars(b, fmt, offset, font_size)
% Place a value label above each bar in a grouped bar chart.
% Uses XEndPoints / YEndPoints (requires MATLAB R2019b or later).
    for k = 1:numel(b)
        xt = b(k).XEndPoints;
        yt = b(k).YEndPoints;
        for m = 1:numel(xt)
            text(xt(m), yt(m) + offset, sprintf(fmt, b(k).YData(m)), ...
                'HorizontalAlignment', 'center', ...
                'VerticalAlignment',   'bottom', ...
                'FontSize',     font_size, ...
                'FontWeight',   'bold');
        end
    end
end
