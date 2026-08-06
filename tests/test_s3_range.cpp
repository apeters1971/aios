#include "http/s3_range.hpp"
#include <gtest/gtest.h>

#include <iostream>
#include <string>


TEST(S3Range, Basic) {
  using namespace aios;
  // Single closed range
  {
    auto r = parse_s3_byte_ranges("bytes=0-3", 10);
    EXPECT_TRUE(r.present && !r.unsatisfiable && r.ranges.size() == 1) << "single present";
    EXPECT_TRUE(r.ranges[0].start == 0 && r.ranges[0].end == 3) << "single span";
  }

  // Open end + suffix
  {
    auto open = parse_s3_byte_ranges("bytes=5-", 10);
    EXPECT_TRUE(open.ranges.size() == 1 && open.ranges[0].start == 5 && open.ranges[0].end == 9) << "open end";
    auto suf = parse_s3_byte_ranges("bytes=-3", 10);
    EXPECT_TRUE(suf.ranges.size() == 1 && suf.ranges[0].start == 7 && suf.ranges[0].end == 9) << "suffix";
  }

  // Multi-range: keep satisfiable only
  {
    auto r = parse_s3_byte_ranges("bytes=0-1, 4-5, 100-200", 10);
    EXPECT_TRUE(r.present && r.ranges.size() == 2) << "multi two ok";
    EXPECT_TRUE(r.ranges[0].start == 0 && r.ranges[0].end == 1) << "multi first";
    EXPECT_TRUE(r.ranges[1].start == 4 && r.ranges[1].end == 5) << "multi second";
  }

  // All unsatisfiable
  {
    auto r = parse_s3_byte_ranges("bytes=50-60", 10);
    EXPECT_TRUE(r.present && r.unsatisfiable && r.ranges.empty()) << "416 case";
  }

  // Clamp end past EOF
  {
    auto r = parse_s3_byte_ranges("bytes=8-999", 10);
    EXPECT_TRUE(r.ranges.size() == 1 && r.ranges[0].start == 8 && r.ranges[0].end == 9) << "clamp end";
  }

  // Ignore non-bytes / garbage
  {
    EXPECT_TRUE(!parse_s3_byte_ranges("items=0-1", 10).present) << "non-bytes ignored";
    EXPECT_TRUE(!parse_s3_byte_ranges("", 10).present) << "empty ignored";
  }

  // Multipart body shape
  {
    std::vector<S3ByteRange> ranges{{0, 1}, {4, 5}};
    std::vector<std::string> parts{"AB", "EF"};
    auto body = build_s3_multipart_byteranges("BOUND", "text/plain", 10, ranges, parts);
    EXPECT_TRUE(body.find("--BOUND\r\n") != std::string::npos) << "boundary open";
    EXPECT_TRUE(body.find("Content-Type: text/plain\r\n") != std::string::npos) << "part ctype";
    EXPECT_TRUE(body.find("Content-Range: bytes 0-1/10\r\n") != std::string::npos) << "part range0";
    EXPECT_TRUE(body.find("Content-Range: bytes 4-5/10\r\n") != std::string::npos) << "part range1";
    EXPECT_TRUE(body.find("\r\n\r\nAB\r\n") != std::string::npos) << "part0 data";
    EXPECT_TRUE(body.find("\r\n\r\nEF\r\n") != std::string::npos) << "part1 data";
    EXPECT_TRUE(body.find("--BOUND--\r\n") != std::string::npos) << "closing boundary";
  }

  }
