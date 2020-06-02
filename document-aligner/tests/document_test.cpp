#include <bitset>
#include <cassert>
#include <vector>
#include <iostream>
#include <immintrin.h>

using namespace std;

struct Document {
	vector<uint64_t> ngrams;
	vector<float> tfidfs;
};

float intersect_prod(Document const &left, Document const &right)
{
	auto lnit  = left.ngrams.data(),
	     lnend = left.ngrams.data() + left.ngrams.size(),
	     rnit  = right.ngrams.data(),
	     rnend = right.ngrams.data() + right.ngrams.size();

	auto lfit  = left.tfidfs.data(),
	     rfit  = right.tfidfs.data();

	float sum = 0;

	while (lnit != lnend && rnit != rnend) {
		if (*lnit < *rnit) {
			++lnit;
			++lfit;
		} else if (*rnit < *lnit) {
			++rnit;
			++rfit;
		} else {
			assert(*lnit == *rnit);
			sum += *lfit * *rfit;
			++lnit;
			++lfit;
			++rnit;
			++rfit;
		}
	}

	return sum;
}

float intersect_prod_cd(Document &left, Document &right)
{
	auto lnit  = left.ngrams.data(),
	     lnend = left.ngrams.data() + left.ngrams.size(),
	     rnit  = right.ngrams.data(),
	     rnend = right.ngrams.data() + right.ngrams.size();

	auto lfit  = left.tfidfs.data(),
	     rfit  = right.tfidfs.data();

	float sum = 0;

	__m512i in;

	while (lnit != lnend && rnit != rnend) {
		// get a chunk of the left & right ngrams in a registry
		in = _mm512_mask_loadu_epi64(in, (__mmask8) 0x0F, lnit-4);
		in = _mm512_mask_loadu_epi64(in, (__mmask8) 0xF0, rnit);
		__m512i out = _mm512_conflict_epi64(in);

		uint64_t tmp[8];
		_mm512_store_epi64(tmp, out);
		cout << bitset<512>(tmp) << endl;
		break;
	}

	return sum;
}

int main(int argc, char* argv[]) {
	Document left = {
		.ngrams={  0,   1,   2,   3,   4,   5,   6,   7,   8},
		.tfidfs={0.0, 0.1, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8}
	};

	Document right = {
		.ngrams={  0,   1,   3,   4,   6,   9,  10,  11,  12},
		.tfidfs={0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5, 0.5}
	};

	float out_expected = 0.0 * 0.5 + 0.1 * 0.5 + 0.3 * 0.5 + 0.4 * 0.5 + 0.6 * 0.5;
	float out_real = intersect_prod_cd(left, right);

	cout << "expected " << out_expected << "\n"
	     << "     got " << out_real     << "\n";

	return out_expected == out_real ? 0 : 1;
}