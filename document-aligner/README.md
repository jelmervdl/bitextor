# Document aligner & friends

Kitchen-sink example:
```
gzip -cd is/sentences_en.gz | b64filter tokenize | gzip -c is/tokenized_en.gz

< en/sentences.gz | docenc -d | tokenize | docenc | gzip -c en/tokenised.gz

docalign is/tokenised_en.gz en/tokenised.gz \
    | cut -f2 -f3 \
    | docjoin -li -ri -l is/sentences.gz -r en/sentences.gz -l is/sentences_en.gz \
    | parallel --gnu --pipe bluealign-cpp \
    | gzip -c \
    > aligned.gz
```

# Tools
Besides docalign and it's little companion tool docjoin there are a couple more tools in here to work with base64-encoded documents.

- **docalign**: Give it two (optionally compressed) files with base64-encoded tokenised documents, and it will tell you how well each of the documents in the two files match up. Output is scores + document indices. To be used with docjoin.
- **docjoin**: Take two sets of input files, and merge their lines into multiple columns based on index pairs provided to stdin.
- **docenc**: Encode (or decode) sentences into documents. Sentences are grouped in documents by separating batches of sentences by a document marker. This can be either an empty line (i.e. \n, like HTTP) or \0 (when using the -0 flag). Reminder for myself: encode (the default) combines sentences into documents. Decode explodes documents into sentences. Sentences are always split by newlines, documents either by blank lines or null bytes.
- **b64filter**: Wraps a program and passes all lines from all documents through. Think of `< sentences.gz b64filter cat` as `< sentences.gz docenc -d | cat | docenc`. Difference is that it doesn't pass any document separators to the delegate program, it just counts how many lines go in and gathers that many lines at the output side of it. C++ reimplementation of [b64filter](https://github.com/paracrawl/b64filter)
- **foldfilter**: Wraps a program and passes limited length lines to it. Think b64filter + [fold](https://linux.die.net/man/1/fold). Useful when you want to feed garbage to your MT system but don't want it to go mad on extremely long lines, while also not just chopping off those lines in the hope there might be useful content in there.

# docalign
```
Usage: docalign TRANSLATED-TOKENS ENGLISH-TOKENS

Additional options:
  --help                  produce help message
  --df-sample-rate arg    set sample rate to every n-th document (default: 1)
  -n [ --ngram_size ] arg ngram size (default: 2)
  -j [ --jobs ] arg       set number of threads (default: all)
  --threshold arg         set score threshold (default: 0.1)
  --min_count arg         minimal number of documents an ngram can appear in to
                          be included in DF (default: 2)
  --max_count arg         maximum number of documents for ngram to to appear in
                          (default: 1000)
  --best arg              only output the best match for each document
                          (default: on)
  -v [ --verbose ]        show additional output
```

It is advisable to pass in --df-sample-rate to reduce start-up time and memory
usage for the document frequency part of TFIDF. 1 indicates that every document
will be read while 4 would mean that one of every four documents will be added
to the DF.

## Input
Two files (gzip-compressed or plain text) with on each line a single base64-
encoded list of tokens (separated by whitespace).

## Output
For each alignment score that is greater or equal to the threshold it prints the
score, and the indexes (starting with 1) of the documents in TRANSLATED-TOKENS
and ENGLISH-TOKENS, separated by tabs to STDOUT.

# docjoin
```
Usage: bin/docjoin [ -l filename | -r filename | -li | -ri ] ...
Input via stdin: <left index> "\t" <right index> "\n"

This program joins rows from two sets of files into tab-separated output.

Column options:
  -l    Use left index for the following files
  -r    Use right index for the following files
  -li   Print the left index
  -ri   Print the right index

The order of the columns in the output is the same as the order of the
arguments given to the program.
```

# docenc
```
Usage: docenc [ -d ] [ -0 ] [ -q | -v ] [ -n ] [ index ... ] [ file ... ]

The indexes can be specified as a single index or a range in the form INT-INT.
You can specify multiple indices or ranges at once. The rest of the arguments
are interpreted as files to encode/decode.

Arguments:
  -d      Decode mode: convert base64 into plain text. If not specified it will
          default to encode mode.
  -0      Use null byte instead of double newline as document separator.
  -q      Do not complain if the document separator is encountered in the output
          while decoding.
  -v      Print how many documents were encoded/decoded to stderr.
  -n      When decoding, prefix each line with the document index.

Modes:
  encode  Interpret the input as plain text documents that need to be base64
          encoded. The document separator (double newline) will cause a new
          document to be generated.
  decode  Interpret each line as a base64 encoded document that needs to be
          converted to plain text. The plain text will have double newlines
          (or null bytes) to indicate a document has ended and the next one
          begins.
```

Better served by an example:
```
docenc -d < plain_text.gz \
	| tr '[:lower:]' '[:upper:]' \
	| bin/docenc \
	| gzip -c \
	> loud_text.gz
```

Pretty similar to what can be achieved with something like the following set
on a Linux machine (macOS base64 will not accept multiple lines):
```
zcat $@ | head -nX | tail -nY | base64 -d
```

Mostly useful for debugging, but can also be useful to convert plain text into
base64-encoded documents, i.e. converting single line html into multi-line
documents:
```
cat lines_of_html.tsv \
  | cut -d$'\t' -f2 \
  | sed -r 's/$/\x0/g' \
  | sed -r 's/<br\/>|<\/p><p>/\n/g' \
  | sed -r 's/<\/?p>//g' \
  | docenc -0 \
  > sentences.gz
```

# b64filter
```
Usage: b64filter command [ args... ]
```

Again, an example shows much more:
```
gzip -cd plain_text.gz \
	| b64filter tr '[:lower:]' '[:upper:]' \
	| gzip -c \
	> very_loud_text.gz
```

# foldfilter
Think of it as a wrapper version of [fold](https://linux.die.net/man/1/fold).

Wrap an MT system that does not like long sentences and this tool chops those
lines (assuming each line is a single sentence) temporarily into multiple lines.
It uses some heuristics to determine where to break up lines, but if it can't
find a good break point it will still just chop words in half.

```
Usage: foldfilter [ -w INT ] [ -d DELIMITERS ] [ -s ] command [ args ... ]

Arguments:
  -w INT         Split input lines into lines of at most INT bytes. Default: 80
  -d DELIMITERS  Use these delimiters instead of the default set `:, -./`. Note
                 that the order specified here is also the order of preference
                 when breaking a line. The first delimiter of this set that
                 breaks the line into a width smaller/equal to the wanted width
                 will be selected, not the last! I.e. with the default set it
                 will prefer to break on white space instead of breaking up a
                 word with a dash in it. The default set assumes the input is
                 already split into a single sentence per line, and `.` and `/`
                 are mainly here to break up URIs.
  -s             Skip sending the delimiters to the wrapped command. If added,
                 delimiters around the breaking point (both before and after)
                 will be pass directly to the output, and not to the wrapped
                 command. Useful if you do not trust the wrapped program to not
                 trim them off. Delimiters inside lines, i.e. that are not at
                 the beginning or end of a line are always sent.
```

The program's exit code is that of the wrapped command, or 1 if the arguments
could not be interpreted or an error occurred, i.e. invalid utf8 was passed in.

**utf8 safe:** This tool won't break up unicode characters, and you can use
unicode characters as delimiters.


# Building on CSD3
```
module load gcc
module load cmake
```

Compile boost (available boost seems to not link properly).
```
wget https://dl.bintray.com/boostorg/release/1.72.0/source/boost_1_72_0.tar.gz
tar -xzf boost_1_72_0.tar.gz
cd boost_1_72_0.tar.gz
./bootstrap.sh --prefix=$HOME/.local
./b2 install
```

Compile dalign with the up-to-date Boost
```
cd bitextor/document-aligner
mkdir build
cd build
cmake -D Boost_DIR=$HOME/.local/lib/cmake/Boost-1.72.0 ..
make -j4
```

Now you should have a `bin/docalign` and others in your build directory. Note that it is
linked to your custom Boost which makes it a bit less portable.
