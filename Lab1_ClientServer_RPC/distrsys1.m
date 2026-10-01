% =========================================================
% DATA INITIALIZATION
% =========================================================

% X-Axis: Number of Concurrent Clients (Powers of 2)
clients = [1, 2, 4, 8, 16, 32, 64];

% --- 1-Worker Data ---
mean_1w = [18.07, 25.38, 41.32, 72.66, 133.79, 255.70, 501.60];
std_1w  = [0.45,  0.62,  1.55,  2.66,  2.57,   5.16,   3.88];
min_1w  = [17.10, 24.50, 38.80, 67.20, 129.20, 247.20, 495.70];
max_1w  = [18.70, 26.70, 44.00, 76.60, 136.50, 263.40, 505.10];
thru_1w = [55.3,  78.8,  96.8,  110.1, 119.6,  125.1,  127.6];

% --- 2-Worker Data ---
mean_2w = [18.01, 17.55, 25.63, 41.08, 73.73,  134.46, 264.53];
std_2w  = [0.67,  0.85,  1.25,  2.83,  3.92,   6.80,   3.03];
min_2w  = [16.30, 15.30, 24.50, 36.50, 67.70,  126.80, 258.40];
max_2w  = [18.80, 18.40, 28.80, 45.90, 79.80,  145.10, 267.50];
thru_2w = [55.5,  114.0, 156.1, 194.7, 217.0,  238.0,  241.9];

% --- 4-Worker Data ---
mean_4w = [17.56, 17.58, 16.88, 24.13, 41.06,  80.90,  161.63];
std_4w  = [0.68,  0.72,  0.66,  1.28,  5.38,   10.12,  3.00];
min_4w  = [16.40, 15.70, 15.90, 22.20, 36.40,  69.60,  156.20];
max_4w  = [18.80, 18.50, 18.00, 26.00, 51.40,  93.40,  165.10];
thru_4w = [56.9,  113.8, 237.0, 331.5, 389.6,  395.5,  396.0];

% --- 8-Worker Data ---
mean_8w = [17.92, 17.53, 16.20, 21.91, 33.60,  57.86,  93.72];
std_8w  = [0.28,  0.74,  1.64,  2.01,  2.40,   4.66,   2.61];
min_8w  = [17.50, 16.10, 14.10, 19.00, 30.00,  52.30,  89.40];
max_8w  = [18.40, 18.70, 18.50, 25.00, 36.80,  67.30,  95.90];
thru_8w = [55.8,  114.1, 246.9, 365.1, 476.2,  553.0,  682.9];

% --- 16-Worker Data ---
mean_16w = [17.89, 17.23, 16.69, 22.28, 32.03,  54.44,  96.15];
std_16w  = [0.70,  2.27,  1.96,  1.97,  1.63,   2.55,   7.65];
min_16w  = [16.90, 11.50, 12.00, 20.00, 29.60,  52.20,  87.20];
max_16w  = [19.20, 19.50, 19.00, 25.90, 34.40,  58.50,  111.40];
thru_16w = [55.9,  116.1, 239.7, 359.1, 499.5,  588.8,  665.6];

% --- Multi-Server Null Request Metrics ---
% (a) Overhead comparison
null_types = {'Single Server', 'Multi-Server + LB (3)'};
null_means = [0.864, 1.087]; 
null_stds  = [0.317, 0.428];

% =========================================================
% GENERATE PLOTS (Logarithmic X-Axis base 2)
% =========================================================

% 1. Mean Total Time Plot
figure(1);
plot(clients, mean_1w, '-o', 'LineWidth', 2, 'DisplayName', '1 Worker');
hold on;
plot(clients, mean_2w, '-s', 'LineWidth', 2, 'DisplayName', '2 Workers');
plot(clients, mean_4w, '-^', 'LineWidth', 2, 'DisplayName', '4 Workers');
plot(clients, mean_8w, '-d', 'LineWidth', 2, 'DisplayName', '8 Workers');
plot(clients, mean_16w, '-p', 'LineWidth', 2, 'DisplayName', '16 Workers');
set(gca, 'XScale', 'log');  
xticks(clients);            
grid on;
xlabel('Number of Concurrent Clients (Log Scale)');
ylabel('Mean Total Time (ms)');
title('Mean Total Processing Time vs. Client Load');
legend('Location', 'northwest');

% 2. Standard Deviation Plot
figure(2);
plot(clients, std_1w, '-o', 'LineWidth', 2, 'DisplayName', '1 Worker');
hold on;
plot(clients, std_2w, '-s', 'LineWidth', 2, 'DisplayName', '2 Workers');
plot(clients, std_4w, '-^', 'LineWidth', 2, 'DisplayName', '4 Workers');
plot(clients, std_8w, '-d', 'LineWidth', 2, 'DisplayName', '8 Workers');
plot(clients, std_16w, '-p', 'LineWidth', 2, 'DisplayName', '16 Workers');
set(gca, 'XScale', 'log');
xticks(clients);
grid on;
xlabel('Number of Concurrent Clients (Log Scale)');
ylabel('Standard Deviation (ms)');
title('Processing Time Variance (Std Dev) vs. Client Load');
legend('Location', 'northwest');

% 3. Minimum Time Plot
figure(3);
plot(clients, min_1w, '-o', 'LineWidth', 2, 'DisplayName', '1 Worker');
hold on;
plot(clients, min_2w, '-s', 'LineWidth', 2, 'DisplayName', '2 Workers');
plot(clients, min_4w, '-^', 'LineWidth', 2, 'DisplayName', '4 Workers');
plot(clients, min_8w, '-d', 'LineWidth', 2, 'DisplayName', '8 Workers');
plot(clients, min_16w, '-p', 'LineWidth', 2, 'DisplayName', '16 Workers');
set(gca, 'XScale', 'log');
xticks(clients);
grid on;
xlabel('Number of Concurrent Clients (Log Scale)');
ylabel('Minimum Time (ms)');
title('Minimum Processing Time vs. Client Load');
legend('Location', 'northwest');

% 4. Maximum Time Plot
figure(4);
plot(clients, max_1w, '-o', 'LineWidth', 2, 'DisplayName', '1 Worker');
hold on;
plot(clients, max_2w, '-s', 'LineWidth', 2, 'DisplayName', '2 Workers');
plot(clients, max_4w, '-^', 'LineWidth', 2, 'DisplayName', '4 Workers');
plot(clients, max_8w, '-d', 'LineWidth', 2, 'DisplayName', '8 Workers');
plot(clients, max_16w, '-p', 'LineWidth', 2, 'DisplayName', '16 Workers');
set(gca, 'XScale', 'log');
xticks(clients);
grid on;
xlabel('Number of Concurrent Clients (Log Scale)');
ylabel('Maximum Time (ms)');
title('Maximum Processing Time vs. Client Load');
legend('Location', 'northwest');

% 5. Average Throughput Plot
figure(5);
plot(clients, thru_1w, '-o', 'LineWidth', 2, 'DisplayName', '1 Worker');
hold on;
plot(clients, thru_2w, '-s', 'LineWidth', 2, 'DisplayName', '2 Workers');
plot(clients, thru_4w, '-^', 'LineWidth', 2, 'DisplayName', '4 Workers');
plot(clients, thru_8w, '-d', 'LineWidth', 2, 'DisplayName', '8 Workers');
plot(clients, thru_16w, '-p', 'LineWidth', 2, 'DisplayName', '16 Workers');
set(gca, 'XScale', 'log');
xticks(clients);
grid on;
xlabel('Number of Concurrent Clients (Log Scale)');
ylabel('Throughput (Requests / Second)');
title('Server Throughput vs. Client Load');
legend('Location', 'northwest');

% 6. Null Request Overhead
figure(6);
b = bar(null_means, 0.4); % 0.4 makes the bars narrower
b.FaceColor = 'flat';
b.CData(1,:) = [0.2 0.6 0.5]; % Single Server: Teal/Green
b.CData(2,:) = [0.1 0.4 0.8]; % Multi Server: Royal Blue
hold on;
errorbar(1:2, null_means, null_stds, 'k', 'linestyle', 'none', 'LineWidth', 1.5);
set(gca, 'xticklabel', null_types);
ylabel('Latency (ms)'); title('Load Balancing Protocol Overhead Analysis');
grid on;
% Precision text labels on top of bars
text(1:2, null_means + 0.05, string(null_means) + " ms", 'Horiz', 'center', 'FontWeight', 'bold');
% Display the delta as a subtitle
lb_tax = null_means(2) - null_means(1);
subtitle(sprintf('Load Balancing Latency Tax: +%.3f ms', lb_tax));