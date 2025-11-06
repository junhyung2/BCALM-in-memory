#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <iterator>
#include <ctime>
#include <unordered_map>
#include <algorithm>
#include <sys/stat.h>
#include <sys/types.h>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <iomanip> // setprecision, fixed

#include <cstdlib> // system
#include <cstdio>  // ::remove
#include <memory>  // unique_ptr

#include "lm.h"

/*
 * Constuct the compacted de bruijn graph from list of distinct kmers
 */

using namespace std;

// 10 chars to store the integer hash of a minimizer
#define MINIMIZER_STR_SIZE 10

// keep largest bucket seen, disregarding small ones
unsigned long nb_elts_in_largest_bucket = 10000;

string minimizer2string(int input_int)
{
	long long i = input_int;
	string str = to_string(i);
	assert(str.size() <= MINIMIZER_STR_SIZE);
	if (MINIMIZER_STR_SIZE > str.size())
		str.insert(0, MINIMIZER_STR_SIZE - str.size(), '0');
	return str;
}

bool nextchar(char *c)
{
	switch (*c)
	{
	case 'a':
		*c = 'c';
		return true;
	case 'c':
		*c = 'g';
		return true;
	case 'g':
		*c = 't';
		return true;
	case 't':
		*c = 'a';
		return false;
	default:
		cout << "Problem with nextchar: " << c;
		assert(0);
		return false;
	}
}

// ================= IN-MEMORY VFS (faithful to original file I/O semantics) =================
// 목표: 알고리즘/흐름/경계/문자열 포맷을 "원본 그대로" 유지하고,
//       .bcalmtmp/* 임시 파일만 메모리 내에 보관한다.

struct MemFile
{
	std::string buf;
	int64_t pos = 0;
	bool exists = false;
};
static std::unordered_map<std::string, MemFile> MEMFS;

static inline void memfs_clear_dir(const std::string &prefix)
{
	std::vector<std::string> to_del;
	to_del.reserve(MEMFS.size());
	for (auto &kv : MEMFS)
		if (kv.first.rfind(prefix, 0) == 0)
			to_del.push_back(kv.first);
	for (auto &k : to_del)
		MEMFS.erase(k);
}
static inline void memfs_remove(const std::string &path) { MEMFS.erase(path); }
static inline bool memfs_exists(const std::string &path) { return MEMFS.find(path) != MEMFS.end(); }

class VIn
{
public:
	std::string path;
	MemFile *mf;
	VIn(const std::string &p) : path(p)
	{
		auto it = MEMFS.find(p);
		mf = (it == MEMFS.end() ? nullptr : &it->second);
		if (mf)
			mf->pos = 0;
	}
	bool is_open() const { return (mf != nullptr); }
	void seekg(int64_t newpos, std::ios_base::seekdir dir = std::ios::beg)
	{
		if (!mf)
			return;
		if (dir == std::ios::beg)
			mf->pos = newpos;
		else if (dir == std::ios::cur)
			mf->pos += newpos;
		else if (dir == std::ios::end)
			mf->pos = (int64_t)mf->buf.size() + newpos;
		if (mf->pos < 0)
			mf->pos = 0;
		if (mf->pos > (int64_t)mf->buf.size())
			mf->pos = mf->buf.size();
	}
	int64_t tellg() const { return (mf ? mf->pos : -1); }
	std::string readn_local(int64_t n)
	{
		if (!mf || n <= 0)
			return std::string();
		int64_t remain = (int64_t)mf->buf.size() - mf->pos;
		int64_t take = std::min(remain, n);
		std::string s = mf->buf.substr((size_t)mf->pos, (size_t)take);
		mf->pos += take;
		return s;
	}
};

class VOut
{
public:
	std::string path;
	MemFile *mf;
	bool append_mode;
	VOut(const std::string &p, bool app) : path(p), append_mode(app)
	{
		auto &f = MEMFS[p];
		mf = &f;
		if (!f.exists)
		{
			f.exists = true;
			f.pos = 0;
			f.buf.clear();
		}
		if (append_mode)
		{
			f.pos = f.buf.size();
		}
		else
		{
			f.pos = 0;
			f.buf.clear();
		}
	}
	VOut &operator<<(const std::string &s)
	{
		if (mf)
		{
			mf->buf.append(s);
			mf->pos = mf->buf.size();
		}
		return *this;
	}
	VOut &operator<<(const char *s)
	{
		if (mf)
		{
			mf->buf.append(s);
			mf->pos = mf->buf.size();
		}
		return *this;
	}
	VOut &operator<<(char c)
	{
		if (mf)
		{
			mf->buf.push_back(c);
			mf->pos = mf->buf.size();
		}
		return *this;
	}
};

// 원본 lm.cpp의 유틸리티 시그니처를 VFS에 맞게 오버로드
static inline std::string readn(VIn *in, int64_t n) { return in->readn_local(n); }
static inline void copylm(VIn *in, int64_t n, VOut *out)
{
	int64_t buffsize = 1000000, nbbuffer = n / buffsize;
	for (int64_t j = 0; j < nbbuffer; j++)
		*out << readn(in, buffsize);
	*out << readn(in, n - nbbuffer * buffsize);
}
// reversecompletment는 ograph.h 에 선언/정의가 있으므로 재선언 금지
static inline void copylmrv(VIn *in, int64_t n, VOut *out)
{
	int64_t buffsize = 1000000, nbbuffer = n / buffsize;
	int64_t pos = in->tellg();
	for (int64_t j = 1; j <= nbbuffer; j++)
	{
		in->seekg(pos + n - j * buffsize, std::ios::beg);
		*out << reversecompletment(readn(in, buffsize));
	}
	int64_t rest = n - nbbuffer * buffsize;
	if (rest != 0)
	{
		in->seekg(pos, std::ios::beg);
		*out << reversecompletment(readn(in, rest));
	}
}
// ================= END IN-MEMORY VFS =================

// reads k-mers and Put kmers in superbuckets
// (other behavior: just count m-mers)
void sortentry(string namefile, const int k, const int m, bool create_buckets, bool m_mer_count)
{
	int numbersuperbucket(pow(4, m));
	std::vector<std::unique_ptr<VOut>> out(numbersuperbucket);

	if (create_buckets)
	{
		for (long long i(0); i < numbersuperbucket; i++)
			out[i].reset(new VOut(".bcalmtmp/z" + to_string(i), /*append*/ true));
	}

	ifstream in(namefile);
	string kmer, waste;

	while (1)
	{
		getline(in, kmer, ' ');
		getline(in, waste);

		if (kmer.size() < (unsigned int)k)
		{
			break;
		}
		transform(kmer.begin(), kmer.end(), kmer.begin(), ::tolower);

		if (m_mer_count)
		{
			count_m_mers(kmer, 2 * m, k);
			continue;
		}

		int middlemin, leftmostmin, rightmostmin, min;

		middlemin = minimiserrc(kmer.substr(1, k - 2), 2 * m);
		leftmostmin = minimiserrc(kmer.substr(0, 2 * m), 2 * m);
		rightmostmin = minimiserrc(kmer.substr(kmer.size() - 2 * m, 2 * m), 2 * m);

		int leftmin = (leftmostmin < middlemin) ? leftmostmin : middlemin;

		int rightmin = (rightmostmin < middlemin) ? rightmostmin : middlemin;

		min = (leftmin < rightmin) ? leftmin : rightmin;

		uint64_t h = min / numbersuperbucket;

		if (create_buckets)
			*out[h] << kmer << minimizer2string(leftmin) << minimizer2string(rightmin) << ";";
		else
			cout << h << ":" << kmer << minimizer2string(leftmin) << minimizer2string(rightmin) << ";\n";
	}
	if (create_buckets)
		cout << "initial partitioning done" << endl;
}

// Put nodes from superbuckets to buckets
void createbucket(const string superbucketname, const int m)
{
	int superbucketnum(stoi(superbucketname));
	VIn in(".bcalmtmp/z" + superbucketname);
	if (!in.is_open())
	{
		cerr << "Problem with Createbucket" << endl;
		return;
	}
	in.seekg(0, ios_base::end);
	int64_t size(in.tellg()), buffsize(1000000), numberbuffer(size / buffsize), nb(pow(4, m)), suffix;
	if (size == 0)
	{
		memfs_remove(".bcalmtmp/z" + superbucketname);
		return;
	}
	in.seekg(0, ios::beg);
	std::vector<std::unique_ptr<VOut>> out(nb);
	for (long long i(0); i < nb; i++)
	{
		out[i].reset(new VOut(".bcalmtmp/" + to_string(superbucketnum * nb + i), /*append*/ true));
	}
	int64_t lastposition(-1), position(0), point(0), mini;
	string buffer;
	vector<string> miniv;
	in.seekg(0);

	for (int j(0); j <= numberbuffer; j++)
	{
		if (j == numberbuffer)
		{
			if (size - numberbuffer * buffsize - 1 != -1)
			{
				buffer = readn(&in, size - numberbuffer * buffsize - 1);
				buffer += ";";
			}
			else
				buffer = "";
		}
		else
		{
			buffer = readn(&in, buffsize);
			point += buffsize;
		}
		for (uint64_t i(0); i < buffer.size(); i++, position++)
		{
			if ((buffer)[i] == ';')
			{
				int leftmin, rightmin;
				if (i >= (uint64_t)(MINIMIZER_STR_SIZE * 2))
				{
					leftmin = stoi(buffer.substr(i - (MINIMIZER_STR_SIZE * 2), MINIMIZER_STR_SIZE));
					rightmin = stoi(buffer.substr(i - MINIMIZER_STR_SIZE, MINIMIZER_STR_SIZE));
				}
				else
				{
					in.seekg(position - (MINIMIZER_STR_SIZE * 2));
					leftmin = stoi(readn(&in, MINIMIZER_STR_SIZE));
					rightmin = stoi(readn(&in, MINIMIZER_STR_SIZE));
				}
				string first_bucket_of_superbucket(to_string((long long)(superbucketnum * nb - 1)));
				mini = minbutbiggerthan(leftmin, rightmin, first_bucket_of_superbucket);
				suffix = mini % nb;
				in.seekg(lastposition + 1, ios_base::beg);
				copylm(&in, position - lastposition, out[suffix].get());
				lastposition = position;
			}
		}
		in.seekg(point);
	}
	memfs_remove(".bcalmtmp/z" + superbucketname);
}

// count the length of each node
vector<int64_t> countbucket(const string &name)
{
	vector<int64_t> count;
	VIn in(".bcalmtmp/" + name);
	if (in.is_open())
	{
		in.seekg(0, ios_base::end);
		int64_t size(in.tellg()), buffsize(10), numberbuffer(size / buffsize), lastposition(-1), position(0);
		if (size < 2)
			return count;
		in.seekg(0, ios::beg);
		string buffer;

		for (int j(0); j < numberbuffer; j++)
		{
			buffer = readn(&in, buffsize);
			for (uint64_t i(0); i < buffer.size(); i++, position++)
				if ((buffer)[i] == ';')
				{
					count.push_back(position - lastposition - 1);
					lastposition = position;
				}
		}

		buffer = readn(&in, size - numberbuffer * buffsize);
		for (uint64_t i(0); i < buffer.size(); i++, position++)
			if ((buffer)[i] == ';')
			{
				count.push_back(position - lastposition - 1);
				lastposition = position;
			}
	}
	return count;
}

// true iff node does not contain tag
bool notag(const string &node, const int64_t start, int64_t *n)
{
	for (uint64_t i(start); i < node.size(); i++)
	{
		if (node[i] >= '0' && node[i] <= '9')
		{
			*n = i;
			return false;
		}
	}
	return true;
}

// return length of tag
int taglength(const string &node, int64_t j)
{
	int n = 1;
	for (uint64_t i(j + 1); i < node.size(); i++)
		if ((node[i] >= '0' && node[i] <= '9') || node[i] == '+' || node[i] == '-')
			n++;
		else
			return n;
	return n;
}

// Write a node remplacing tags by their sequences
void writeit(const string &outfile, const string &node, int leftmin, int rightmin, vector<pair<int64_t, int64_t>> *tagsposition, VIn *tagfile, int64_t j, const string &fout)
{
	VOut out(".bcalmtmp/" + outfile, /*append*/ true);
	char rc;
	int64_t lastposition(0), tag, tagl, position, length;
	pair<int64_t, int64_t> pair;
	do
	{
		out << node.substr(lastposition, j - lastposition - 1);
		tagl = taglength(node, j);
		rc = node[j - 1];
		if (rc == '+')
		{
			tag = stoi(node.substr(j, tagl));
		}
		else
		{
			tag = stoi(reversecompletment(node.substr(j, tagl)));
		}
		lastposition = j + tagl;
		pair = (*tagsposition)[tag];
		position = pair.first;
		length = pair.second;
		tagfile->seekg(position, ios_base::beg);
		if (rc == '+')
			copylm(tagfile, length, &out);
		if (rc == '-')
			copylmrv(tagfile, length, &out);
	} while (!notag(node, lastposition, &j));
	if (outfile != fout)
		out << node.substr(lastposition) << minimizer2string(leftmin) << minimizer2string(rightmin) << ";";
	else
		out << node.substr(lastposition) << ";\n";
}

void put(const string &outfile, const string &node, int leftmin, int rightmin, const string &fout)
{
	VOut out(".bcalmtmp/" + outfile, /*append*/ true);
	if (outfile == fout)
		out << node << ";\n";
	else
		out << node << minimizer2string(leftmin) << minimizer2string(rightmin) << ";";
}

void putorwrite(const string &outfile, const string &node, int leftmin, int rightmin, vector<pair<int64_t, int64_t>> *tagsposition, VIn *tagfile, const string &fout)
{
	int64_t i;
	if (notag(node, 0, &i))
		put(outfile, node, leftmin, rightmin, fout);
	else
		writeit(outfile, node, leftmin, rightmin, tagsposition, tagfile, i, fout);
}

// Decide where to put a node
void goodplace(const string &node, int leftmin, int rightmin, const string &bucketname, vector<pair<int64_t, int64_t>> *tagsposition, VIn *tagfile, const int m, const string &nameout)
{
	int nb(pow(4, m)), prefixnumber(stoi(bucketname) / nb + 1);
	long long mini(minbutbiggerthan(leftmin, rightmin, bucketname));
	if (mini == -1)
		putorwrite(nameout, node, leftmin, rightmin, tagsposition, tagfile, nameout);
	else
	{
		long long minipre(mini / nb);
		string miniprefix('z' + to_string(minipre));
		putorwrite(((minipre >= prefixnumber) ? miniprefix : to_string(mini)), node, leftmin, rightmin, tagsposition, tagfile, nameout);
	}
}

// Compact a bucket and put the nodes on the right place
void compactbucket(const int &prefix, const int &suffix, const int k, const char *nameout, const int m)
{
	int64_t buffsize(k), postags(0), length, nb(pow(4, m));
	long long tagnumber(0), numberbucket(prefix * nb + suffix);
	string fullname(to_string(numberbucket)), node, tag, end;
	auto count(countbucket(fullname));
	if (count.size() == 0)
	{
		memfs_remove(".bcalmtmp/" + fullname);
		return;
	}

	// keep largest bucket seen
	if (count.size() > nb_elts_in_largest_bucket)
	{
		nb_elts_in_largest_bucket = count.size();
		// 디버그용 덤프 (원본의 cp와 대응)
		std::ofstream dbg("largest_bucket.dot", std::ios::binary);
		auto it = MEMFS.find(".bcalmtmp/" + fullname);
		if (dbg && it != MEMFS.end())
			dbg << it->second.buf;
	}

	VIn in(".bcalmtmp/" + fullname);
	VOut tagfile(".bcalmtmp/tags", /*append*/ false), out(".bcalmtmp/" + (string)nameout, /*append*/ true);
	graph g(k);
	vector<pair<int64_t, int64_t>> tagsposition;

	// add nodes to graph
	for (auto it = count.begin(); it != count.end(); it++)
	{
		length = *it;
		if (length - (MINIMIZER_STR_SIZE * 2) <= 2 * buffsize)
		{
			node = readn(&in, length + 1);
			g.addvertex(node.substr(0, length - (MINIMIZER_STR_SIZE * 2)));
			g.addleftmin(stoi(node.substr(length - (MINIMIZER_STR_SIZE * 2), MINIMIZER_STR_SIZE)));
			g.addrightmin(stoi(node.substr(length - MINIMIZER_STR_SIZE, MINIMIZER_STR_SIZE)));
		}
		else
		{
			node = readn(&in, buffsize);
			tag = to_string(tagnumber);
			node += "+" + tag + "+";
			tagnumber++;
			copylm(&in, length - (MINIMIZER_STR_SIZE * 2) - 2 * buffsize, &tagfile);
			tagsposition.push_back(make_pair(postags, length - (MINIMIZER_STR_SIZE * 2) - 2 * buffsize));
			postags += length - (MINIMIZER_STR_SIZE * 2) - 2 * buffsize;
			end = readn(&in, buffsize);
			node += end.substr(0, buffsize);
			g.addvertex(node);
			g.addleftmin(stoi(readn(&in, MINIMIZER_STR_SIZE)));
			g.addrightmin(stoi(readn(&in, MINIMIZER_STR_SIZE)));
			readn(&in, 1); // the ';'
		}
	}
	memfs_remove(".bcalmtmp/" + fullname);
	g.debruijn();

	g.compressh(stoi(fullname));
	VIn fichiertagin(".bcalmtmp/tags");
	int node_index = 0;

	for (auto it(g.nodes.begin()); it != g.nodes.end(); it++)
	{
		if (it->size() != 0)
		{
			int leftmin = g.leftmins[node_index];
			int rightmin = g.rightmins[node_index];
			goodplace(*it, leftmin, rightmin, fullname, &tagsposition, &fichiertagin, m, nameout);
		}
		node_index++;
	}

	memfs_remove(".bcalmtmp/tags");
	return;
}

// Create a file with the nodes of the compacted graph
void createoutfile(const char *namein, const char *nameout, const int k, const int m)
{
	auto start = std::chrono::steady_clock::now(); // modified
	HashMap hm(build_hash_map(2 * m));
	int64_t nbsuperbucket(pow(4, m));

	// ".bcalmtmp" 클린업 (메모리)
	memfs_clear_dir(".bcalmtmp/");
	std::remove(nameout);

	// create the hash function
	init_m_mers_table(2 * m);
	sortentry(namein, k, m, false, true);
	create_hash_function_from_m_mers(2 * m);

	sortentry(namein, k, m);
	for (long long i(0); i < nbsuperbucket; i++)
	{
		createbucket(to_string(i), m);
		for (int j(0); j < pow(4, m); j++)
			compactbucket(i, j, k, nameout, m);
	}

	// 최종 결과 .bcalmtmp/nameout -> 실제 nameout 파일로 "mv"에 해당
	{
		auto it = MEMFS.find(std::string(".bcalmtmp/") + nameout);
		if (it != MEMFS.end())
		{
			std::ofstream out(nameout, std::ios::binary);
			out << it->second.buf;
		}
		else
		{
			std::cerr << "system call failed" << std::endl;
		}
	}

	auto end = std::chrono::steady_clock::now();
	std::chrono::duration<double> waitedFor = end - start;
	std::cout << std::fixed << std::setprecision(3)
			  << "Last for " << waitedFor.count() << " s" << std::endl;
}
