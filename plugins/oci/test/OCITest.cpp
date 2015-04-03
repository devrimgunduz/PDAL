/******************************************************************************
* Copyright (c) 2011, Michael P. Gerlek (mpg@flaxen.com)
*
* All rights reserved.
*
* Redistribution and use in source and binary forms, with or without
* modification, are permitted provided that the following
* conditions are met:
*
*     * Redistributions of source code must retain the above copyright
*       notice, this list of conditions and the following disclaimer.
*     * Redistributions in binary form must reproduce the above copyright
*       notice, this list of conditions and the following disclaimer in
*       the documentation and/or other materials provided
*       with the distribution.
*     * Neither the name of Hobu, Inc. or Flaxen Geo Consulting nor the
*       names of its contributors may be used to endorse or promote
*       products derived from this software without specific prior
*       written permission.
*
* THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
* "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
* LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
* FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
* COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
* INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
* BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
* OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
* AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
* OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
* OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
* OF SUCH DAMAGE.
****************************************************************************/

#include "gtest/gtest.h"

#include <boost/property_tree/ptree.hpp>

#include <pdal/Filter.hpp>
#include <pdal/StageFactory.hpp>
#include <las/LasReader.hpp>

#include "../io/OciCommon.hpp"
#include "Support.hpp"
#include "TestConfig.hpp"

using namespace pdal;

GTEST_API_ int main(int argc, char **argv)
{
    testing::InitGoogleTest(&argc, argv);
    for (int i = 0; i < argc; ++i)
    {
        std::string s = argv[i];
        if (Utils::startsWith(s, "--connection"))
        {
            auto pos = s.find_first_of('=');
            if (pos == string::npos)
                throw pdal_error("Invalid command line connection string.");
            TestConfig::g_oracle_connection = s.substr(pos + 1);
            break;
        }
    }
    return RUN_ALL_TESTS();
}


bool ShouldRunTest()
{
    return TestConfig::g_oracle_connection.size() > 0;
}


Options getOCIOptions()
{
    Options options;

    options.add("overwrite", false);
    options.add("connection", std::string(TestConfig::g_oracle_connection));
    options.add("block_table_name", "PDAL_TEST_BLOCKS");
    options.add("base_table_name", "PDAL_TEST_BASE");
    options.add("cloud_column_name", "CLOUD");
    options.add("srid", 26910);
    options.add("disable_cloud_trigger", true);
    options.add("filename", Support::datapath("autzen/autzen-utm.las"));
    options.add("xml_schema_dump", "pcs-oracle-xml-schema-dump.xml");
    return options;
}

class SplitFilter : public Filter
{
public:
   virtual std::string getName() const
       { return "split_filter"; }

private:
    virtual PointViewSet run(PointViewPtr view)
    {
        PointViewSet out;
        PointViewPtr v = view->makeNew();
        for (PointId i = 0; i < view->size(); ++i)
        {
            if (i && (i % 100 == 0))
            {
                out.insert(v);
                v = v->makeNew();
            }
            v->appendPoint(*view, i);
        }
        out.insert(v);
        return out;
    }
};

class OCITest : public testing::Test
{
protected:
    virtual void SetUp()
    {
        m_options = getOCIOptions();

        connect();
        cleanup();

        std::string base_table_name =
            m_options.getValueOrThrow<std::string>("base_table_name");
        std::string create_pc_table("CREATE TABLE " + base_table_name +
            " (id number, CLOUD SDO_PC, DESCRIPTION VARCHAR2(20), HEADER "
            "BLOB, BOUNDARY SDO_GEOMETRY)");
        run(create_pc_table);

        std::string block_table_name =
            m_options.getValueOrThrow<std::string>("block_table_name");
        std::string create_block_table = "CREATE TABLE " + block_table_name +
            " AS SELECT * FROM MDSYS.SDO_PC_BLK_TABLE";
        run(create_block_table);
    }

    void connect()
    {
        if (!m_connection.get())
            m_connection = pdal::connect(TestConfig::g_oracle_connection);
        if (!m_connection.get() || !m_connection->Succeeded())
        {
            std::ostringstream oss;

            oss << "Couldn't connect via OCI using spec '" <<
                TestConfig::g_oracle_connection << "'";
            throw pdal_error(oss.str());
        }
    }

    void run(std::string sql)
    {
        Statement statement(m_connection->CreateStatement(sql.c_str()));
        statement->Execute();
    }

    Options m_options;
    Connection m_connection;

    virtual void TearDown()
    {
    }

    void cleanup()
    {
        std::string base_table_name =
            m_options.getValueOrThrow<std::string>("base_table_name");
        std::string block_table_name =
            m_options.getValueOrThrow<std::string>("block_table_name");

        std::string drop_base_table = "DROP TABLE " + base_table_name;
        std::string drop_block_table = "DROP TABLE " + block_table_name;
        run(drop_base_table);
        run(drop_block_table);

        std::string cleanup_metadata = "DELETE FROM USER_SDO_GEOM_METADATA "
            "WHERE TABLE_NAME ='" + block_table_name + "'";
        run(cleanup_metadata);
    }

};


void writeData(Orientation::Enum orient, bool scaling)
{
    Options options;

    options.add("capacity", 10000);
    options.add("connection", std::string(TestConfig::g_oracle_connection));
    options.add("debug", "true");
    options.add("block_table_name", "PDAL_TEST_BLOCKS");
    options.add("base_table_name", "PDAL_TEST_BASE");
    options.add("cloud_column_name", "CLOUD");
    options.add("srid", 26910);
    options.add("disable_cloud_trigger", true);
    options.add("store_dimensional_orientation",
        orient == Orientation::DimensionMajor);
    options.add("filename", Support::datapath("autzen/autzen-utm.las"));
    if (scaling)
    {
        options.add("offset_x", "auto");
        options.add("offset_y", "auto");
        options.add("offset_z", "auto");
        options.add("scale_x", 1e-6);
        options.add("scale_y", 1e-6);
        options.add("scale_z", 1e-6);
    }

    PointTable table;

    StageFactory f;
    LasReader reader;
    reader.setOptions(options);

    SplitFilter split;
    split.setInput(reader);

    std::unique_ptr<Stage> writer(f.createStage("writers.oci"));
    EXPECT_TRUE(writer.get());
    writer->setOptions(options);
    writer->setInput(split);

    writer->prepare(table);
    writer->execute(table);
}


// Compare the source LAS file with the extracted OCI data.
// Candidate is the OCI reader's view.
void compare(const PointViewPtr candidate, std::string filename)
{
    Options options;
    Option fn("filename", filename);
    options.add(fn);

    PointTable table;

    LasReader reader;
    reader.setOptions(options);

    reader.prepare(table);
    PointViewSet viewSet = reader.execute(table);

    EXPECT_EQ(viewSet.size(), 1u);
    PointViewPtr source = *viewSet.begin();

    EXPECT_EQ(source->size(), candidate->size());
    PointId limit = std::min(source->size(), candidate->size());
    
    for (PointId i = 0; i < limit; ++i)
    {
        using namespace Dimension;

        int32_t sx = source->getFieldAs<int32_t>(Id::X, i);
        int32_t sy = source->getFieldAs<int32_t>(Id::Y, i);
        int32_t sz = source->getFieldAs<int32_t>(Id::Z, i);
        uint16_t sintensity = source->getFieldAs<uint16_t>(Id::Intensity, i);
        uint16_t sred = source->getFieldAs<uint16_t>(Id::Red, i);
        uint16_t sgreen = source->getFieldAs<uint16_t>(Id::Green, i);
        uint16_t sblue = source->getFieldAs<uint16_t>(Id::Blue, i);

        int32_t cx = candidate->getFieldAs<int32_t>(Id::X, i);
        int32_t cy = candidate->getFieldAs<int32_t>(Id::Y, i);
        int32_t cz = candidate->getFieldAs<int32_t>(Id::Z, i);
        uint16_t cintensity = candidate->getFieldAs<uint16_t>(Id::Intensity, i);
        uint16_t cred = candidate->getFieldAs<uint16_t>(Id::Red, i);
        uint16_t cgreen = candidate->getFieldAs<uint16_t>(Id::Green, i);
        uint16_t cblue = candidate->getFieldAs<uint16_t>(Id::Blue, i);

        EXPECT_EQ(sx, cx);
        EXPECT_EQ(sy, cy);
        EXPECT_EQ(sz, cz);
        EXPECT_EQ(sintensity, cintensity);
        EXPECT_EQ(sred, cred);
        EXPECT_EQ(sgreen, cgreen);
        EXPECT_EQ(sblue, cblue);
    }
}


void readData()
{
    std::ostringstream oss;

    oss << "SELECT  l.\"OBJ_ID\", l.\"BLK_ID\", l.\"BLK_EXTENT\", " <<
        "l.\"BLK_DOMAIN\", l.\"PCBLK_MIN_RES\", l.\"PCBLK_MAX_RES\", " <<
        "l.\"NUM_POINTS\", l.\"NUM_UNSORTED_POINTS\", l.\"PT_SORT_DIM\", " <<
        "l.\"POINTS\", b.cloud "
           "FROM PDAL_TEST_BLOCKS l, PDAL_TEST_BASE b "
        "WHERE b.id = l.obj_id ORDER BY l.blk_id ";

    Options options = getOCIOptions();
    options.add("query", oss.str());

    StageFactory f;
    std::unique_ptr<Stage> reader(f.createStage("readers.oci"));
    EXPECT_TRUE(reader.get());

    reader->setOptions(options);

    PointTable table;
    reader->prepare(table);
    PointViewSet viewSet = reader->execute(table);

    EXPECT_EQ(viewSet.size(), 1u);
    PointViewPtr view = *viewSet.begin();
    EXPECT_EQ(view->size(), 1065u);

    compare(view, Support::datapath("autzen/autzen-utm.las"));
}


TEST_F(OCITest, dim_major_unscaled)
{
    writeData(Orientation::DimensionMajor, false);
    readData();
}


TEST_F(OCITest, dim_major_scaled)
{
    writeData(Orientation::DimensionMajor, true);
    readData();
}


TEST_F(OCITest, point_major_unscaled)
{
    writeData(Orientation::PointMajor, false);
    readData();
}


TEST_F(OCITest, point_major_scaled)
{
    writeData(Orientation::PointMajor, true);
    readData();
}


// TEST_F(OCITest, read_view_reproj)
// {
//     if (!ShouldRunTest()) return;
//
//     WriteDefaultData();
//
//     std::ostringstream oss;
//
//     oss << "SELECT  l.\"OBJ_ID\", l.\"BLK_ID\", l.\"BLK_EXTENT\", l.\"BLK_DOMAIN\","
//            "        l.\"PCBLK_MIN_RES\", l.\"PCBLK_MAX_RES\", l.\"NUM_POINTS\","
//            "        l.\"NUM_UNSORTED_POINTS\", l.\"PT_SORT_DIM\", l.\"POINTS\", b.cloud "
//            "FROM PDAL_TEST_BLOCKS l, PDAL_TEST_BASE b "
//         "WHERE b.id=1 and l.obj_id = b.id "
//         "ORDER BY l.obj_id ";
//     Options options = getOCIOptions();
//     Option& query = options.getOptionByRef("query");
//     query.setValue<std::string>(oss.str());
//
//     Option& out_srs = options.getOptionByRef("out_srs");
//     out_srs.setValue<std::string>("EPSG:26910");
//
//     Option& x_scale = options.getOptionByRef("scale_x");
//     x_scale.setValue<float>(0.01);
//
//     Option& y_scale = options.getOptionByRef("scale_y");
//     y_scale.setValue<float>(0.01);
//
//     // Option& offset_x = options.getOptionByRef("offset_x");
//     // offset_x.setValue<float>( -123.0749695f);
//     // offset_x.setValue<float>( 0.0f);
//
//     // Option& offset_y = options.getOptionByRef("offset_y");
//     // offset_y.setValue<float>( 44.0500086f);
//     // offset_y.setValue<float>( 0.0f);
//
//
//     Option x_dim("x_dim", std::string("readers.oci.X"), "Dimension name to use for 'X' data");
//     Option y_dim("y_dim", std::string("readers.oci.Y"), "Dimension name to use for 'Y' data");
//     Option z_dim("z_dim", std::string("readers.oci.Z"), "Dimension name to use for 'Z' data");
//
//     options.add(x_dim);
//     options.add(y_dim);
//     options.add(z_dim);
//
//     Option& debug = options.getOptionByRef("debug");
//     debug.setValue<std::string>( "true");
//
//     OciReader reader_reader(options);
//     // filters::InPlaceReprojection reproj(reader_reader, options);
//     reader_reader.prepare();
//
//     PointViewPtr data(reader_reader.getSchema(), chunk_size+30);
//     StageSequentialIterator* iter = reader_reader.createSequentialIterator(data);
//
//     uint32_t numRead(0);
//
//     numRead = iter->read(data);
//
//     EXPECT_EQ(numRead, chunk_size+30u);
//     EXPECT_EQ(view->getNumPoints(), chunk_size+30u);
//
//     checkPoints(data);
//
//     delete iter;
//
// }

//
// TEST_F(OCITest, read_all)
// {
//     if (!ShouldRunTest()) return;
//
//     WriteDefaultData();
//     WriteDefaultData();
//
//     std::ostringstream oss;
//
//     oss << "SELECT CLOUD FROM PDAL_TEST_BASE";
//     Options options = getOCIOptions();
//     Option& query = options.getOptionByRef("query");
//     query.setValue<std::string>(oss.str());
//     OciReader reader_reader(options);
//     reader_reader.prepare();
//
//     PointViewPtr data(reader_reader.getSchema(), 2500);
//     std::unique_ptr<StageSequentialIterator> iter(reader_reader.createSequentialIterator(data));
//
//
//     uint32_t numRead = iter->read(data);
//
//     EXPECT_EQ(numRead, 2130u);
//
//     Schema const& schema = view->getSchema();
//     Dimension const& dimX = schema.getDimension("X");
//     Dimension const& dimY = schema.getDimension("Y");
//     Dimension const& dimZ = schema.getDimension("Z");
//     Dimension const& dimIntensity = schema.getDimension("Intensity");
//     Dimension const& dimRed = schema.getDimension("Red");
//
//     int32_t x = view->getFieldAs<int32_t>(dimX, 0);
//     int32_t y = view->getFieldAs<int32_t>(dimY, 0);
//     int32_t z = view->getFieldAs<int32_t>(dimZ, 0);
//     uint16_t intensity = view->getFieldAs<uint16_t>(dimIntensity, 6);
//     uint16_t red = view->getFieldAs<uint16_t>(dimRed, 6);
//
//     EXPECT_EQ(x, -1250367506);
//     EXPECT_EQ(y, 492519663);
//     EXPECT_EQ(z, 12931);
//     EXPECT_EQ(intensity, 67);
//     EXPECT_EQ(red, 113);
//
// }
//

// TEST_F(OCITest, read_view)
// {
//     if (!ShouldRunTest()) return;
//
//     WriteDefaultData();
//
//     std::ostringstream oss;
//
//     oss << "SELECT  l.\"OBJ_ID\", l.\"BLK_ID\", l.\"BLK_EXTENT\", l.\"BLK_DOMAIN\","
//            "        l.\"PCBLK_MIN_RES\", l.\"PCBLK_MAX_RES\", l.\"NUM_POINTS\","
//            "        l.\"NUM_UNSORTED_POINTS\", l.\"PT_SORT_DIM\", l.\"POINTS\", b.cloud "
//            "FROM PDAL_TEST_BLOCKS l, PDAL_TEST_BASE b "
//         "WHERE b.id=1 and l.obj_id = b.id "
//         "ORDER BY l.obj_id ";
//     Options options = getOCIOptions();
//     Option& query = options.getOptionByRef("query");
//     query.setValue<std::string>(oss.str());
//     OciReader reader_reader(options);
//     reader_reader.prepare();
//
//     PointViewPtr data(reader_reader.getSchema(), chunk_size+30);
//     std::unique_ptr<StageSequentialIterator> iter(reader_reader.createSequentialIterator(data));
//
//
//     uint32_t numTotal(0);
//     uint32_t numRead(0);
//
//     PointViewPtr data3(reader_reader.getSchema(), 100);
//     numRead = iter->read(data3);
//     EXPECT_EQ(numRead, 100u);
//     numTotal = numRead + numTotal;
//
//     while (numRead !=0)
//     {
//         numRead = iter->read(data3);
//         numTotal = numRead + numTotal;
//
//     }
//
//     EXPECT_EQ(numRead, 0u);
//     EXPECT_EQ(numTotal, 1065u);
//
// }
