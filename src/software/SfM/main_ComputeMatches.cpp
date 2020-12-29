// This file is part of OpenMVG, an Open Multiple View Geometry C++ library.

// Copyright (c) 2012, 2019 Pierre MOULON, Romuald Perrot.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/graph/graph.hpp"
#include "openMVG/graph/graph_stats.hpp"
#include "openMVG/features/akaze/image_describer_akaze.hpp"
#include "openMVG/features/descriptor.hpp"
#include "openMVG/features/feature.hpp"
#include "openMVG/graph/graph.hpp"
#include "openMVG/matching/indMatch.hpp"
#include "openMVG/matching/indMatch_utils.hpp"
#include "openMVG/matching/pairwiseAdjacencyDisplay.hpp"
#include "openMVG/matching_image_collection/Cascade_Hashing_Matcher_Regions.hpp"
#include "openMVG/matching_image_collection/E_ACRobust.hpp"
#include "openMVG/matching_image_collection/E_ACRobust_Angular.hpp"
#include "openMVG/matching_image_collection/Eo_Robust.hpp"
#include "openMVG/matching_image_collection/F_ACRobust.hpp"
#include "openMVG/matching_image_collection/GeometricFilter.hpp"
#include "openMVG/matching_image_collection/H_ACRobust.hpp"
#include "openMVG/matching_image_collection/Matcher_Regions.hpp"
#include "openMVG/matching_image_collection/Pair_Builder.hpp"
#include "openMVG/sfm/pipelines/sfm_features_provider.hpp"
#include "openMVG/sfm/pipelines/sfm_regions_provider.hpp"
#include "openMVG/sfm/pipelines/sfm_regions_provider_cache.hpp"
#include "openMVG/sfm/sfm_data.hpp"
#include "openMVG/sfm/sfm_data_io.hpp"
#include "openMVG/stl/stl.hpp"
#include "openMVG/system/timer.hpp"

#include "third_party/cmdLine/cmdLine.h"
#include "third_party/stlplus3/filesystemSimplified/file_system.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

using namespace openMVG;
using namespace openMVG::matching;
using namespace openMVG::robust;
using namespace openMVG::sfm;
using namespace openMVG::matching_image_collection;
using namespace std;

enum EGeometricModel
{
  FUNDAMENTAL_MATRIX       = 0,
  ESSENTIAL_MATRIX         = 1,
  HOMOGRAPHY_MATRIX        = 2,
  ESSENTIAL_MATRIX_ANGULAR = 3,
  ESSENTIAL_MATRIX_ORTHO   = 4,
  ESSENTIAL_MATRIX_UPRIGHT = 5
};

/// Compute corresponding features between a series of views:
/// - Load view images description (regions: features & descriptors)
/// - Compute putative local feature matches (descriptors matching)
int main( int argc, char** argv )
{
  CmdLine cmd;

  std::string  sSfM_Data_Filename;
  std::string  sOutputFilename        = "";
  float        fDistRatio             = 0.8f;
  std::string  sPredefinedPairList    = "";
  std::string  sNearestMatchingMethod = "AUTO";
  bool         bForce                 = false;
  unsigned int ui_max_cache_size      = 0;

  //required
  cmd.add( make_option( 'i', sSfM_Data_Filename, "input_file" ) );
  cmd.add( make_option( 'o', sOutputFilename, "out_dir" ) );
  cmd.add( make_option( 'p', sPredefinedPairList, "pair_list" ) );
  // Options
  cmd.add( make_option( 'r', fDistRatio, "ratio" ) );
  cmd.add( make_option( 'n', sNearestMatchingMethod, "nearest_matching_method" ) );
  cmd.add( make_option( 'f', bForce, "force" ) );
  cmd.add( make_option( 'c', ui_max_cache_size, "cache_size" ) );

  try
  {
    if ( argc == 1 )
      throw std::string( "Invalid command line parameter." );
    cmd.process( argc, argv );
  }
  catch ( const std::string& s )
  {
    std::cerr << "Usage: " << argv[ 0 ] << '\n'
              << "[-i|--input_file]   A SfM_Data file\n"
              << "[-o|--output_file]  Output file where computed matches are stored\n"
              << "[-p]--pair_list]    Pairs list file\n"
              << "\n[Optional]\n"
              << "[-f|--force] Force to recompute data]\n"
              << "[-r|--ratio] Distance ratio to discard non meaningful matches\n"
              << "   0.8: (default).\n"
                    << "[-n|--nearest_matching_method]\n"
              << "  AUTO: auto choice from regions type,\n"
              << "  For Scalar based regions descriptor:\n"
              << "    BRUTEFORCEL2: L2 BruteForce matching,\n"
              << "    HNSWL2: L2 Approximate Matching with Hierarchical Navigable Small World graphs,\n"
              << "    ANNL2: L2 Approximate Nearest Neighbor matching,\n"
              << "    CASCADEHASHINGL2: L2 Cascade Hashing matching.\n"
              << "    FASTCASCADEHASHINGL2: (default)\n"
              << "      L2 Cascade Hashing with precomputed hashed regions\n"
              << "     (faster than CASCADEHASHINGL2 but use more memory).\n"
              << "  For Binary based descriptor:\n"
              << "    BRUTEFORCEHAMMING: BruteForce Hamming matching.\n"
              << "[-c|--cache_size]\n"
              << "  Use a regions cache (only cache_size regions will be stored in memory)\n"
              << "  If not used, all regions will be load in memory."
              << std::endl;

    std::cerr << s << std::endl;
    return EXIT_FAILURE;
  }

  std::cout << " You called : "
            << "\n"
            << argv[ 0 ] << "\n"
            << "--input_file " << sSfM_Data_Filename << "\n"
            << "--output_file " << sOutputFilename << "\n"
            << "--pair_list " << sPredefinedPairList << "\n"
            << "Optional parameters:"
            << "\n"
            << "--force " << bForce << "\n"
            << "--ratio " << fDistRatio << "\n"
            << "--nearest_matching_method " << sNearestMatchingMethod << "\n"
            << "--cache_size " << ( ( ui_max_cache_size == 0 ) ? "unlimited" : std::to_string( ui_max_cache_size ) ) << std::endl;

  if ( sPredefinedPairList.empty() )
  {
    std::cerr << "\nNo input pairs file set." << std::endl;
    return EXIT_FAILURE;
  }
  if ( sOutputFilename.empty() )
  {
    std::cerr << "\nNo output file set." << std::endl;
    return EXIT_FAILURE;
  }

  // -----------------------------
  // . Load SfM_Data Views & intrinsics data
  // . Compute putative descriptor matches
  // + Export some statistics
  // -----------------------------

  //---------------------------------------
  // Read SfM Scene (image view & intrinsics data)
  //---------------------------------------
  SfM_Data sfm_data;
  if ( !Load( sfm_data, sSfM_Data_Filename, ESfM_Data( VIEWS | INTRINSICS ) ) )
  {
    std::cerr << std::endl
              << "The input SfM_Data file \"" << sSfM_Data_Filename << "\" cannot be read." << std::endl;
    return EXIT_FAILURE;
  }
  const std::string sMatchesDirectory = stlplus::folder_part( sOutputFilename );

  //---------------------------------------
  // Load SfM Scene regions
  //---------------------------------------
  // Init the regions_type from the image describer file (used for image regions extraction)
  using namespace openMVG::features;
  const std::string        sImage_describer = stlplus::create_filespec( sMatchesDirectory, "image_describer", "json" );
  std::unique_ptr<Regions> regions_type     = Init_region_type_from_file( sImage_describer );
  if ( !regions_type )
  {
    std::cerr << "Invalid: "
              << sImage_describer << " regions type file." << std::endl;
    return EXIT_FAILURE;
  }

  //---------------------------------------
  // a. Compute putative descriptor matches
  //    - Descriptor matching (according user method choice)
  //    - Keep correspondences only if NearestNeighbor ratio is ok
  //---------------------------------------

  // Load the corresponding view regions
  std::shared_ptr<Regions_Provider> regions_provider;
  if ( ui_max_cache_size == 0 )
  {
    // Default regions provider (load & store all regions in memory)
    regions_provider = std::make_shared<Regions_Provider>();
  }
  else
  {
    // Cached regions provider (load & store regions on demand)
    regions_provider = std::make_shared<Regions_Provider_Cache>( ui_max_cache_size );
  }

  // Show the progress on the command line:
  C_Progress_display progress;

  if ( !regions_provider->load( sfm_data, sMatchesDirectory, regions_type, &progress ) )
  {
    std::cerr << std::endl
              << "Invalid regions." << std::endl;
    return EXIT_FAILURE;
  }

  PairWiseMatches map_PutativesMatches;

  // Build some alias from SfM_Data Views data:
  // - List views as a vector of filenames & image sizes
  std::vector<std::string>               vec_fileNames;
  std::vector<std::pair<size_t, size_t>> vec_imagesSize;
  {
    vec_fileNames.reserve( sfm_data.GetViews().size() );
    vec_imagesSize.reserve( sfm_data.GetViews().size() );
    for ( Views::const_iterator iter = sfm_data.GetViews().begin();
          iter != sfm_data.GetViews().end();
          ++iter )
    {
      const View* v = iter->second.get();
      vec_fileNames.push_back( stlplus::create_filespec( sfm_data.s_root_path,
                                                         v->s_Img_path ) );
      vec_imagesSize.push_back( std::make_pair( v->ui_width, v->ui_height ) );
    }
  }

  std::cout << std::endl
            << " - PUTATIVE MATCHES - " << std::endl;
  // If the matches already exists, reload them
  if ( !bForce && ( stlplus::file_exists( sOutputFilename ) ) )
  {
    if ( !( Load( map_PutativesMatches, sOutputFilename ) ) )
    {
      std::cerr << "Cannot load input matches file";
      return EXIT_FAILURE;
    }
    std::cout << "\t PREVIOUS RESULTS LOADED;"
              << " #pair: " << map_PutativesMatches.size() << std::endl;
  }
  else // Compute the putative matches
  {
    // Allocate the right Matcher according the Matching requested method
    std::unique_ptr<Matcher> collectionMatcher;
    if ( sNearestMatchingMethod == "AUTO" )
    {
      if ( regions_type->IsScalar() )
      {
        std::cout << "Using FAST_CASCADE_HASHING_L2 matcher" << std::endl;
        collectionMatcher.reset( new Cascade_Hashing_Matcher_Regions( fDistRatio ) );
      }
      else if ( regions_type->IsBinary() )
      {
        std::cout << "Using BRUTE_FORCE_HAMMING matcher" << std::endl;
        collectionMatcher.reset( new Matcher_Regions( fDistRatio, BRUTE_FORCE_HAMMING ) );
      }
    }
    else if ( sNearestMatchingMethod == "BRUTEFORCEL2" )
    {
      std::cout << "Using BRUTE_FORCE_L2 matcher" << std::endl;
      collectionMatcher.reset( new Matcher_Regions( fDistRatio, BRUTE_FORCE_L2 ) );
    }
    else if ( sNearestMatchingMethod == "BRUTEFORCEHAMMING" )
    {
      std::cout << "Using BRUTE_FORCE_HAMMING matcher" << std::endl;
      collectionMatcher.reset( new Matcher_Regions( fDistRatio, BRUTE_FORCE_HAMMING ) );
    }
    else if ( sNearestMatchingMethod == "HNSWL2" )
    {
      std::cout << "Using HNSWL2 matcher" << std::endl;
      collectionMatcher.reset(new Matcher_Regions(fDistRatio, HNSW_L2));
    }
    else if ( sNearestMatchingMethod == "ANNL2" )
    {
      std::cout << "Using ANN_L2 matcher" << std::endl;
      collectionMatcher.reset( new Matcher_Regions( fDistRatio, ANN_L2 ) );
    }
    else if ( sNearestMatchingMethod == "CASCADEHASHINGL2" )
    {
      std::cout << "Using CASCADE_HASHING_L2 matcher" << std::endl;
      collectionMatcher.reset( new Matcher_Regions( fDistRatio, CASCADE_HASHING_L2 ) );
    }
    else if ( sNearestMatchingMethod == "FASTCASCADEHASHINGL2" )
    {
      std::cout << "Using FAST_CASCADE_HASHING_L2 matcher" << std::endl;
      collectionMatcher.reset( new Cascade_Hashing_Matcher_Regions( fDistRatio ) );
    }
    if ( !collectionMatcher )
    {
      std::cerr << "Invalid Nearest Neighbor method: " << sNearestMatchingMethod << std::endl;
      return EXIT_FAILURE;
    }
    // Perform the matching
    system::Timer timer;
    {
      // From matching mode compute the pair list that have to be matched:
      Pair_Set pairs;
      if ( !loadPairs( sfm_data.GetViews().size(), sPredefinedPairList, pairs ) )
      {
        std::cerr << "Failed to load pairs from file: \"" << sPredefinedPairList << "\"" << std::endl;
        return EXIT_FAILURE;
      }
      // Photometric matching of putative pairs
      collectionMatcher->Match( regions_provider, pairs, map_PutativesMatches, &progress );
      //---------------------------------------
      //-- Export putative matches
      //---------------------------------------
      if ( !Save( map_PutativesMatches, std::string( sOutputFilename ) ) )
      {
        std::cerr << "Cannot save computed matches in: "
                  << sOutputFilename << std::endl;
        return EXIT_FAILURE;
      }
    }
    std::cout << "Task (Regions Matching) done in (s): " << timer.elapsed() << std::endl;
  }
  //-- export putative matches Adjacency matrix
  PairWiseMatchingToAdjacencyMatrixSVG( vec_fileNames.size(),
                                        map_PutativesMatches,
                                        stlplus::create_filespec( sMatchesDirectory, "PutativeAdjacencyMatrix", "svg" ) );
  //-- export view pair graph once putative graph matches have been computed
  {
    std::set<IndexT> set_ViewIds;
    std::transform( sfm_data.GetViews().begin(), sfm_data.GetViews().end(), std::inserter( set_ViewIds, set_ViewIds.begin() ), stl::RetrieveKey() );
    graph::indexedGraph putativeGraph( set_ViewIds, getPairs( map_PutativesMatches ) );
    graph::exportToGraphvizData(
        stlplus::create_filespec( sMatchesDirectory, "putative_matches" ),
        putativeGraph );
  }

  return EXIT_SUCCESS;
}
