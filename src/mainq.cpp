#include <ostream>
#include <chrono>

#include <liboscar/StaticOsmCompleter.h>
#include <sserialize/stats/TimeMeasuerer.h>
#include <sserialize/stats/statfuncs.h>
#include "static-htm-index.h"

struct Config {
    std::string oscarFiles;
    std::string htmFiles;
};

struct WorkData {
    WorkData() {}
    virtual ~WorkData() {}
    template<typename T>
    T const * as() const { return dynamic_cast<T const *>(this); }

    template<typename T>
    T * as() { return dynamic_cast<T*>(this); }
};

template<typename T>
struct WorkDataSingleValue: public WorkData {
    WorkDataSingleValue(T const & value) : value(value) {}
    virtual ~WorkDataSingleValue() override {}
    T value;
};



using WorkDataString = WorkDataSingleValue<std::string>;
using WorkDataQueryFile = WorkDataSingleValue<std::string>;
using WorkDataU32 = WorkDataSingleValue<uint32_t>;

struct WorkItem {
    enum Type {
        WI_QUERY_STRING,
		WI_QUERY_FILE,
		WI_NUM_THREADS,
        WI_HTM_QUERY,
        WI_OSCAR_QUERY,
        WI_PRELOAD
    };

    WorkItem(Type t, WorkData * d) : data(d), type(t) {}

    std::unique_ptr<WorkData> data;
    Type type;
};

struct State {
    std::vector<WorkItem> queue;
    std::string str;
	uint32_t numThreads{1};
};

struct HtmState {
    sserialize::UByteArrayAdapter indexData;
    sserialize::UByteArrayAdapter searchData;
    sserialize::Static::ItemIndexStore idxStore;
};

struct Completers {
    std::shared_ptr<liboscar::Static::OsmCompleter> cmp;
	std::shared_ptr<hic::Static::OscarSearchSgCompleter> sgcmp;
};

struct QueryStats {
	sserialize::CellQueryResult cqr;
	sserialize::ItemIndex items;
	sserialize::TimeMeasurer cqrTime;
	sserialize::TimeMeasurer flatenTime;
};

template<typename T_OUTPUT_ITERATOR>
void readCompletionStringsFromFile(const std::string & fileName, T_OUTPUT_ITERATOR out) {
	std::string tmp;
	std::ifstream inFile;
	inFile.open(fileName);
	if (!inFile.is_open()) {
		throw std::runtime_error("Failed to read completion strings from " + fileName);
	}
	while (!inFile.eof()) {
		std::getline(inFile, tmp);
		*out = tmp;
		++out;
	}
	inFile.close();
}

std::ostream & operator<<(std::ostream & out, QueryStats const & ts) {
	out << "# cells: " << ts.cqr.cellCount() << '\n';
	out << "# items: " << ts.items.size() << '\n';
	out << "Cell time: " << ts.cqrTime << '\n';
	out << "Flaten time: " << ts.flatenTime << '\n';
	return out;
}
	
std::string const meas_res_unit{"us"};

struct Stats {
	using meas_res = std::chrono::microseconds;
	std::vector<double> cqr;
	std::vector<double> flaten;
	std::vector<uint32_t> cellCount;
	std::vector<uint32_t> itemCount;
	void reserve(std::size_t size) {
		cqr.reserve(size);
		flaten.reserve(size);
		cellCount.reserve(size);
		itemCount.reserve(size);
	}
};

void doQueryFileCompletions(Completers & completers, std::string const & qfn) {
	std::vector<std::string> queries;
	readCompletionStringsFromFile(qfn, std::back_inserter(queries));
	
	Stats sg_stats, o_stats;
	sg_stats.reserve(queries.size());
	o_stats.reserve(queries.size());
	
	sserialize::ProgressInfo pinfo;
	pinfo.begin(queries.size(), "Computing queries");
	for(std::size_t i(0), s(queries.size()); i < s; ++i) {
		
		auto start = std::chrono::high_resolution_clock::now();
		auto sg_cqr = completers.sgcmp->complete(queries[i]);
		auto stop = std::chrono::high_resolution_clock::now();
		sg_stats.cqr.emplace_back(std::chrono::duration_cast<Stats::meas_res>(stop-start).count());
		
		start = std::chrono::high_resolution_clock::now();
		auto sg_items = sg_cqr.flaten();
		stop = std::chrono::high_resolution_clock::now();
		sg_stats.flaten.emplace_back(std::chrono::duration_cast<Stats::meas_res>(stop-start).count());
		sg_stats.cellCount.emplace_back(sg_cqr.cellCount());
		sg_stats.itemCount.emplace_back(sg_items.size());
		
		start = std::chrono::high_resolution_clock::now();
		auto o_cqr = completers.cmp->cqrComplete(queries[i]);
		stop = std::chrono::high_resolution_clock::now();
		o_stats.cqr.emplace_back(std::chrono::duration_cast<Stats::meas_res>(stop-start).count());
		
		start = std::chrono::high_resolution_clock::now();
		auto o_items = o_cqr.flaten();
		stop = std::chrono::high_resolution_clock::now();
		o_stats.flaten.emplace_back(std::chrono::duration_cast<Stats::meas_res>(stop-start).count());
		o_stats.cellCount.emplace_back(o_cqr.cellCount());
		o_stats.itemCount.emplace_back(o_items.size());
		
		SSERIALIZE_EXPENSIVE_ASSERT(sg_items == o_items);
	}
	pinfo.end();
	std::cout << sg_stats.cqr.back();
	
	using namespace sserialize::statistics;
	std::cout << "SpatialIndex::cqr:" << std::endl;
	StatPrinting::print(std::cout, sg_stats.cqr.begin(), sg_stats.cqr.end());
	std::cout << "SpatialIndex::flaten:" << std::endl;
	StatPrinting::print(std::cout, sg_stats.flaten.begin(), sg_stats.flaten.end());
	std::cout << "SpatialIndex::cellCount:" << std::endl;
	StatPrinting::print(std::cout, sg_stats.cellCount.begin(), sg_stats.cellCount.end());
	
	std::cout << "Oscar::cqr:" << std::endl;
	StatPrinting::print(std::cout, o_stats.cqr.begin(), o_stats.cqr.end());
	std::cout << "Oscar::flaten:" << std::endl;
	StatPrinting::print(std::cout, o_stats.flaten.begin(), o_stats.flaten.end());
	std::cout << "Oscar::cellCount:" << std::endl;
	StatPrinting::print(std::cout, o_stats.cellCount.begin(), o_stats.cellCount.end());
}

void help() {
	std::cerr << "prg -o <oscar files> -f <htm files> -m <query string> -hq -oq --preload" << std::endl;
}

int main(int argc, char const * argv[]) {
    Config cfg;
    State state;
    HtmState htmState;
	Completers completers;

    for(int i(1); i < argc; ++i) {
        std::string token(argv[i]);
        if (token == "-o" && i+1 < argc ) {
            cfg.oscarFiles = std::string(argv[i+1]);
            ++i;
        }
        else if (token == "-f" && i+1 < argc) {
            cfg.htmFiles = std::string(argv[i+1]);
            ++i;
        }
		else if (token == "-m" && i+1 < argc) {
			state.queue.emplace_back(WorkItem::WI_QUERY_STRING, new WorkDataString(std::string(argv[i+1])));
			++i;
		}
		else if (token == "-t" && i+1 < argc) {
			state.queue.emplace_back(WorkItem::WI_NUM_THREADS, new WorkDataU32(std::atoi(argv[i+1])));
			++i;
		}
        else if (token == "-hq") {
            state.queue.emplace_back(WorkItem::WI_HTM_QUERY, std::nullptr_t());
        }
        else if (token == "-oq") {
            state.queue.emplace_back(WorkItem::WI_OSCAR_QUERY, std::nullptr_t());
        }
		else if (token == "--tempdir" && i+1 < argc) {
			token = std::string(argv[i+1]);
			sserialize::UByteArrayAdapter::setFastTempFilePrefix(token);
			sserialize::UByteArrayAdapter::setTempFilePrefix(token);
			++i;
		}
		else if (token == "--preload") {
            state.queue.emplace_back(WorkItem::WI_PRELOAD, std::nullptr_t());
		}
        else {
            std::cerr << "Unkown parameter: " << token << std::endl;
			help();
            return -1;
        }
    }

    completers.cmp = std::make_shared<liboscar::Static::OsmCompleter>();
	completers.sgcmp = std::make_shared<hic::Static::OscarSearchSgCompleter>();
	
    completers.cmp->setAllFilesFromPrefix(cfg.oscarFiles);
    try {
        completers.cmp->energize();
    }
    catch (std::exception const & e) {
        std::cerr << "Error occured: " << e.what() << std::endl;
		help();
		return -1;
    }
    try {
		completers.sgcmp->energize(cfg.htmFiles);
	}
    catch (std::exception const & e) {
        std::cerr << "Error occured: " << e.what() << std::endl;
		help();
		return -1;
    }

	QueryStats oqs, hqs;
	for(uint32_t i(0), s(state.queue.size()); i < s; ++i) {
		WorkItem & wi = state.queue[i];
		
		switch (wi.type) {
			case WorkItem::WI_QUERY_STRING:
				state.str = wi.data->as<WorkDataString>()->value;
				break;
			case WorkItem::WI_NUM_THREADS:
				state.numThreads = wi.data->as<WorkDataU32>()->value;
				break;
			case WorkItem::WI_HTM_QUERY:
			{
				hqs.cqrTime.begin();
				hqs.cqr = completers.sgcmp->complete(state.str);
				hqs.cqrTime.end();
				hqs.flatenTime.begin();
				hqs.items = hqs.cqr.flaten(state.numThreads);
				hqs.flatenTime.end();
				std::cout << "HtmIndex query: " << state.str << std::endl;
				std::cout << hqs << std::endl;
			}
				break;
			case WorkItem::WI_OSCAR_QUERY:
			{
				oqs.cqrTime.begin();
				oqs.cqr = completers.cmp->cqrComplete(state.str);
				oqs.cqrTime.end();
				oqs.flatenTime.begin();
				oqs.items = oqs.cqr.flaten(state.numThreads);
				oqs.flatenTime.end();
				std::cout << "Oscar query: " << state.str << std::endl;
				std::cout << oqs << std::endl;
			}
				break;
			case WorkItem::WI_QUERY_FILE:
				doQueryFileCompletions(completers, wi.data->as<WorkDataQueryFile>()->value);
				break;
			case WorkItem::WI_PRELOAD:
			{
				for(int fc(liboscar::FC_BEGIN), fce(liboscar::FC_END); fc < fce; ++fc) {
					auto data = completers.cmp->data(liboscar::FileConfig(fc));
					data.advice(sserialize::UByteArrayAdapter::AT_LOAD, data.size());
				}
				htmState.indexData.advice(sserialize::UByteArrayAdapter::AT_LOAD, htmState.indexData.size());
				htmState.searchData.advice(sserialize::UByteArrayAdapter::AT_LOAD, htmState.searchData.size());
			}
				break;
			default:
				break;
		}
	}

    return 0;
}
