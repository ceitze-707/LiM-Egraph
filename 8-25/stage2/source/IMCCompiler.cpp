#include "Scheduler.h" // 引入 Scheduler；入口只负责配置每个 benchmark 并调用 NewDSE。

// 程序入口：逐行读取 benchmarks.txt，为每个电路创建独立 Scheduler。
int main()
{
	// [06-ADD] baseline 未显式设定 rand() 种子，重复运行并不构成多 seed
	// 实验。默认 seed=1 保持可复现；IMC_SEED 可用于独立随机重复。
	// 默认 seed 固定为 1，使未设置环境变量时的随机重写轨迹可复查。
	unsigned int seed = 1;
	if (const char* env_seed = std::getenv("IMC_SEED"))
	{
		// atoi 将环境变量文本转整数；无法解析时返回 0，因此下方仍保证非负。
		int requested_seed = std::atoi(env_seed);
		if (requested_seed >= 0)
			seed = static_cast<unsigned int>(requested_seed);
	}
	// 为所有使用 C rand() 的 baseline 重写逻辑设置本轮随机种子。
	std::srand(seed);
	cout << "[06] IMC_SEED=" << seed << "\n";
	if (const char* threads = std::getenv("IMC_THREADS"))
		cout << "[06] IMC_THREADS=" << threads << "\n";
	if (const char* deterministic = std::getenv("IMC_DETERMINISTIC"))
		cout << "[06] IMC_DETERMINISTIC=" << deterministic << "\n";

	// 文件流读取 benchmark 任务列表；每行格式为“路径 [MF 上界]”。默认保持
	// baseline 的 benchmarks.txt；实验可用 IMC_BENCHMARKS_FILE 指向另一份
	// 清单，从而不会改动当前主实验的输入文件。
	string benchmark_file = "benchmarks.txt";
	if (const char* configured = std::getenv("IMC_BENCHMARKS_FILE"))
		if (*configured != '\0') benchmark_file = configured;
	cout << "[06] benchmark_file=" << benchmark_file << "\n";
	ifstream fin;
	fin.open(benchmark_file, ios::in);
	if (!fin)
	{
		cerr << "Cannot open benchmark file: " << benchmark_file << "\n";
		return 1;
	}
	// strLine 保存整行输入；strBench 保存解析出的电路路径。
	string strBench, strLine;
	// nBound 为调度 MF 上界；-1 是“本行未给上界”的哨兵值。
	int nBound;
	// 将整行字符串按空白字段拆分的输入流。
	istringstream sstream;

	// 文件结束前，依次运行每个 benchmark；各轮 Scheduler 相互独立。
	while (getline(fin, strLine))
	{
		// 每轮重新设默认值，防止上一行的上界遗留。
		nBound = -1;
		// 清除 stringstream 上一轮的 eof/fail 状态。
		sstream.clear();
		// 注入当前文本行，作为本轮字段解析输入。
		sstream.str(strLine);
		// 第一个字段是 Verilog/BLIF/AIG 路径；第二个字段若存在则是 MF 上界。
		sstream >> strBench >> nBound;
		// 新建调度器，防止上一 benchmark 的网络与 Pareto 状态污染当前任务。
		Scheduler MyScheduler;
		// 保存名称，以供结果 CSV、日志及诊断信息标识当前电路。
		MyScheduler.m_netlist.m_strBench = strBench;
		// 读入逻辑网络，并由内部 ConfigWithXMG 生成调度节点表。
		MyScheduler.m_netlist.ReadFromFile(strBench);  //XMG netlist (.v / .bliff / .aig)
		// 未提供上界时，以逻辑门数作为宽松的默认限制。
		if (nBound == -1)
			MyScheduler.m_nBound = MyScheduler.m_netlist.m_vecNode.size();
		// 提供了上界时，按 benchmark 文件的显式实验约束运行。
		else
			MyScheduler.m_nBound = nBound;
		// 进入核心 DSE：生成子网候选、调度、回填完整网络并更新 Pareto 集。
		MyScheduler.NewDSE();
	}
	// [06-FIX] 原版无条件 getchar() 会让重定向输出的批处理实验
	// 在所有 DSE 工作已完成后仍永久等待输入，因而看起来像“k=3 卡死”。
	// 默认直接退出；只有用户显式设置 IMC_PAUSE_AT_END 时才保留双击运行的暂停体验。
	if (std::getenv("IMC_PAUSE_AT_END") != nullptr)
		getchar();
	// 显式释放 benchmark 文件句柄。
	fin.close();
	// 0 表示正常结束。
	return 0;
}
