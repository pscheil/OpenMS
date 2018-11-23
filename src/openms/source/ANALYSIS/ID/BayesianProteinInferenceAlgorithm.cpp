// --------------------------------------------------------------------------
//                   OpenMS -- Open-Source Mass Spectrometry
// --------------------------------------------------------------------------
// Copyright The OpenMS Team -- Eberhard Karls University Tuebingen,
// ETH Zurich, and Freie Universitaet Berlin 2002-2017.
//
// This software is released under a three-clause BSD license:
//  * Redistributions of source code must retain the above copyright
//    notice, this list of conditions and the following disclaimer.
//  * Redistributions in binary form must reproduce the above copyright
//    notice, this list of conditions and the following disclaimer in the
//    documentation and/or other materials provided with the distribution.
//  * Neither the name of any author or any participating institution
//    may be used to endorse or promote products derived from this software
//    without specific prior written permission.
// For a full list of authors, refer to the file AUTHORS.
// --------------------------------------------------------------------------
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL ANY OF THE AUTHORS OR THE CONTRIBUTING
// INSTITUTIONS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
// OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
// WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
// OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
// ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// --------------------------------------------------------------------------
// $Maintainer: Julianus Pfeuffer $
// $Authors: Julianus Pfeuffer $
// --------------------------------------------------------------------------
#include <OpenMS/ANALYSIS/ID/BayesianProteinInferenceAlgorithm.h>
#include <OpenMS/ANALYSIS/ID/FalseDiscoveryRate.h>
#include <OpenMS/ANALYSIS/ID/IDBoostGraph.h>
#include <OpenMS/CHEMISTRY/EnzymaticDigestion.h>
#include <OpenMS/DATASTRUCTURES/FASTAContainer.h>
#include <OpenMS/FORMAT/IdXMLFile.h>


#include <set>


using namespace std;

namespace OpenMS
{
  /// Only works if ProteinGroup nodes are present, which is the case when used in this class
  //TODO private
  class BayesianProteinInferenceAlgorithm::AnnotateIndistGroupsFunctor :
      public std::function<void(IDBoostGraph::Graph&)>
  {
  public:
    void operator() (IDBoostGraph::Graph& fg) {
      // this skips CCs with just peps or prots. We only add edges between different types.
      // and if there were no edges, it would not be a CC.
      if (boost::num_vertices(fg) >= 2)
      {
        IDBoostGraph::Graph::vertex_iterator ui, ui_end;
        boost::tie(ui,ui_end) = boost::vertices(fg);

        for (; ui != ui_end; ++ui)
        {
          if (fg[*ui].which() == 1) //prot group
          {
            ProteinIdentification::ProteinGroup pg{};
            pg.probability = -1.0; //init
            IDBoostGraph::Graph::adjacency_iterator nbIt, nbIt_end;
            boost::tie(nbIt, nbIt_end) = boost::adjacent_vertices(*ui, fg);

            ProteinHit *proteinPtr = nullptr;
            for (; nbIt != nbIt_end; ++nbIt)
            {
              if (fg[*nbIt].which() == 0)
              {
                proteinPtr = boost::get<ProteinHit*>(fg[*nbIt]);
                pg.accessions.push_back(proteinPtr->getAccession());
              }
            }
            // TODO this takes score of last protein of group as representative
            // currently the scores should all be the same, so this is okay.
            pg.probability = proteinPtr->getScore();
          }
        }
      }
    }
  };

  /// A functor that specifies what to do on a connected component (IDBoostGraph::FilteredGraph)
  class BayesianProteinInferenceAlgorithm::GraphInferenceFunctor :
      public std::function<void(IDBoostGraph::Graph&)>
  {
  public:
    const Param& param_;

    explicit GraphInferenceFunctor(const Param& param):
        param_(param)
    {}

    void operator() (IDBoostGraph::Graph& fg) {
      //TODO do quick bruteforce calculation if the cc is really small

      // this skips CCs with just peps or prots. We only add edges between different types.
      // and if there were no edges, it would not be a CC.
      if (boost::num_vertices(fg) >= 2)
      {
        MessagePasserFactory<unsigned long> mpf (param_.getValue("model_parameters:pep_emission"),
                                                 param_.getValue("model_parameters:pep_spurious_emission"),
                                                 param_.getValue("model_parameters:prot_prior"),
                                                 1.0); // the p used for marginalization: 1 = sum product, inf = max product
        BetheInferenceGraphBuilder<unsigned long> bigb;

        IDBoostGraph::Graph::vertex_iterator ui, ui_end;
        boost::tie(ui,ui_end) = boost::vertices(fg);

        // Store the IDs of the nodes for which you want the posteriors in the end (usually at least proteins)
        // Maybe later peptides (e.g. for an iterative procedure)
        vector<vector<unsigned long>> posteriorVars;

        // direct neighbors are proteins on the "left" side and peptides on the "right" side
        // TODO use directed graph? But finding "non-strong" connected components in a directed graph is not
        // out of the box supported by boost
        std::vector<IDBoostGraph::vertex_t> in{};
        //std::vector<IDBoostGraph::vertex_t> out{};

        for (; ui != ui_end; ++ui)
        {
          IDBoostGraph::Graph::adjacency_iterator nbIt, nbIt_end;
          boost::tie(nbIt, nbIt_end) = boost::adjacent_vertices(*ui, fg);

          in.clear();
          //out.clear(); // we dont need out edges currently

          for (; nbIt != nbIt_end; ++nbIt)
          {
            if (fg[*nbIt].which() < fg[*ui].which())
            {
              in.push_back(*nbIt);
            }
            /*else
            {
              out.push_back(*nbIt);
            }*/
          }

          //TODO introduce an enum for the types to make it more clear.
          //Or use the static_visitor pattern: You have to pass the vertex with its neighbors as a second arg though.

          if (fg[*ui].which() == 6) // pep hit = psm
          {
            bigb.insert_dependency(mpf.createSumEvidenceFactor(boost::get<PeptideHit*>(fg[*ui])->getPeptideEvidences().size(), in[0], *ui));
            bigb.insert_dependency(mpf.createPeptideEvidenceFactor(*ui, boost::get<PeptideHit*>(fg[*ui])->getScore()));
          }
          else if (fg[*ui].which() == 2) // pep group
          {
            bigb.insert_dependency(mpf.createPeptideProbabilisticAdderFactor(in, *ui));
          }
          else if (fg[*ui].which() == 1) // prot group
          {
            bigb.insert_dependency(mpf.createPeptideProbabilisticAdderFactor(in, *ui));
          }
          else if (fg[*ui].which() == 0) // prot
          {
            //TODO allow an already present prior probability here
            //TODO modify createProteinFactor to start with a modified prior based on the number of missing
            // peptides (later tweak to include conditional prob. for that peptide
            bigb.insert_dependency(mpf.createProteinFactor(*ui));
            posteriorVars.push_back({*ui});
          }
        }

        // create factor graph for Bayesian network
        InferenceGraph<unsigned long> ig = bigb.to_graph();

        //TODO parametrize the type of scheduler.
        PriorityScheduler<unsigned long> scheduler(param_.getValue("loopy_belief_propagation:dampening_lambda"),
                                                   param_.getValue("loopy_belief_propagation:convergence_threshold"),
                                                   param_.getValue("loopy_belief_propagation:max_nr_iterations"));
        scheduler.add_ab_initio_edges(ig);

        BeliefPropagationInferenceEngine<unsigned long> bpie(scheduler, ig);
        auto posteriorFactors = bpie.estimate_posteriors(posteriorVars);

        //TODO you could also save the indices of the peptides here and request + update their posteriors, too.
        for (auto const& posteriorFactor : posteriorFactors)
        {
          double posterior = 0.0;
          IDBoostGraph::SetPosteriorVisitor pv;
          unsigned long nodeId = posteriorFactor.ordered_variables()[0];
          const PMF& pmf = posteriorFactor.pmf();
          // If Index 1 is in the range of this result PMFFactor it is non-zero
          if (1 >= pmf.first_support()[0] && 1 <= pmf.last_support()[0]) {
            posterior = pmf.table()[1 - pmf.first_support()[0]];
          }
          auto bound_visitor = std::bind(pv, std::placeholders::_1, posterior);
          boost::apply_visitor(bound_visitor, fg[nodeId]);
        }
        //TODO we could write out the posteriors here, so we can easily read them for the best params of the grid search

      }
      else
      {
        std::cout << "Skipped cc with only one type (proteins or peptides)" << std::endl;
      }
    }
  };

  /// A functor that specifies what to do on a connected component (IDBoostGraph::FilteredGraph)
  class BayesianProteinInferenceAlgorithm::ExtendedGraphInferenceFunctor :
      public std::function<void(IDBoostGraph::Graph&)>
  {
  public:
    const Param& param_;

    explicit ExtendedGraphInferenceFunctor(const Param& param):
        param_(param)
    {}

    void operator() (IDBoostGraph::Graph& fg) {
      //TODO do quick bruteforce calculation if the cc is really small

      // this skips CCs with just peps or prots. We only add edges between different types.
      // and if there were no edges, it would not be a CC.
      if (boost::num_vertices(fg) >= 2)
      {
        MessagePasserFactory<unsigned long> mpf (param_.getValue("model_parameters:pep_emission"),
                                                 param_.getValue("model_parameters:pep_spurious_emission"),
                                                 param_.getValue("model_parameters:prot_prior"),
                                                 1.0); // the p used for marginalization: 1 = sum product, inf = max product
        BetheInferenceGraphBuilder<unsigned long> bigb;

        IDBoostGraph::Graph::vertex_iterator ui, ui_end;
        boost::tie(ui,ui_end) = boost::vertices(fg);

        // Store the IDs of the nodes for which you want the posteriors in the end (usually at least proteins)
        // Maybe later peptides (e.g. for an iterative procedure)
        vector<vector<unsigned long>> posteriorVars;

        // direct neighbors are proteins on the "left" side and peptides on the "right" side
        // TODO use directed graph? But finding "non-strong" connected components in a directed graph is not
        // out of the box supported by boost
        std::vector<IDBoostGraph::vertex_t> in{};
        //std::vector<IDBoostGraph::vertex_t> out{};

        for (; ui != ui_end; ++ui)
        {
          IDBoostGraph::Graph::adjacency_iterator nbIt, nbIt_end;
          boost::tie(nbIt, nbIt_end) = boost::adjacent_vertices(*ui, fg);

          in.clear();
          //out.clear(); // we dont need out edges currently

          for (; nbIt != nbIt_end; ++nbIt)
          {
            if (fg[*nbIt].which() < fg[*ui].which())
            {
              in.push_back(*nbIt);
            }
            /*else
            {
              out.push_back(*nbIt);
            }*/
          }

          //TODO introduce an enum for the types to make it more clear.
          //Or use the static_visitor pattern: You have to pass the vertex with its neighbors as a second arg though.

          if (fg[*ui].which() == 6) // pep hit = psm
          {
            bigb.insert_dependency(mpf.createSumEvidenceFactor(boost::get<PeptideHit*>(fg[*ui])->getPeptideEvidences().size(), in[0], *ui));
            bigb.insert_dependency(mpf.createPeptideEvidenceFactor(*ui, boost::get<PeptideHit*>(fg[*ui])->getScore()));
          }
          else if (fg[*ui].which() == 2) // pep group
          {
            bigb.insert_dependency(mpf.createPeptideProbabilisticAdderFactor(in, *ui));
          }
          else if (fg[*ui].which() == 1) // prot group
          {
            bigb.insert_dependency(mpf.createPeptideProbabilisticAdderFactor(in, *ui));
          }
          else if (fg[*ui].which() == 0) // prot
          {
            //TODO allow an already present prior probability here
            //TODO modify createProteinFactor to start with a modified prior based on the number of missing
            // peptides (later tweak to include conditional prob. for that peptide
            bigb.insert_dependency(mpf.createProteinFactor(*ui));
            posteriorVars.push_back({*ui});
          }
        }

        // create factor graph for Bayesian network
        InferenceGraph<unsigned long> ig = bigb.to_graph();

        //TODO parametrize the type of scheduler.
        PriorityScheduler<unsigned long> scheduler(param_.getValue("loopy_belief_propagation:dampening_lambda"),
                                                   param_.getValue("loopy_belief_propagation:convergence_threshold"),
                                                   param_.getValue("loopy_belief_propagation:max_nr_iterations"));
        scheduler.add_ab_initio_edges(ig);

        BeliefPropagationInferenceEngine<unsigned long> bpie(scheduler, ig);
        auto posteriorFactors = bpie.estimate_posteriors(posteriorVars);

        //TODO you could also save the indices of the peptides here and request + update their posteriors, too.
        for (auto const& posteriorFactor : posteriorFactors)
        {
          double posterior = 0.0;
          IDBoostGraph::SetPosteriorVisitor pv;
          unsigned long nodeId = posteriorFactor.ordered_variables()[0];
          const PMF& pmf = posteriorFactor.pmf();
          // If Index 1 is in the range of this result PMFFactor it is non-zero
          if (1 >= pmf.first_support()[0] && 1 <= pmf.last_support()[0]) {
            posterior = pmf.table()[1 - pmf.first_support()[0]];
          }
          auto bound_visitor = std::bind(pv, std::placeholders::_1, posterior);
          boost::apply_visitor(bound_visitor, fg[nodeId]);
        }
        //TODO we could write out the posteriors here, so we can easily read them for the best params of the grid search

      }
      else
      {
        std::cout << "Skipped cc with only one type (proteins or peptides)" << std::endl;
      }
    }
  };

  struct BayesianProteinInferenceAlgorithm::GridSearchEvaluator
  {
    Param& param_;
    IDBoostGraph& ibg_;
    const ProteinIdentification& prots_;

    explicit GridSearchEvaluator(Param& param, IDBoostGraph& ibg, const ProteinIdentification& prots):
        param_(param),
        ibg_(ibg),
        prots_(prots)
    {}

    double operator() (double alpha, double beta, double gamma)
    {
      std::cout << "Evaluating: " << alpha << " " << beta << " " << gamma << std::endl;
      param_.setValue("model_parameters:prot_prior", gamma);
      param_.setValue("model_parameters:pep_emission", alpha);
      param_.setValue("model_parameters:pep_spurious_emission", beta);
      ibg_.applyFunctorOnCCs(GraphInferenceFunctor(const_cast<const Param&>(param_)));
      FalseDiscoveryRate fdr;
      return fdr.applyEvaluateProteinIDs(prots_);
    }
  };


  BayesianProteinInferenceAlgorithm::BayesianProteinInferenceAlgorithm() :
      DefaultParamHandler("BayesianProteinInference"),
      ProgressLogger()
  {
    // set default parameter values

    /* More parameter TODOs:
     * - grid search settings: e.g. fine, coarse, prob. threshold, lower convergence crit.
     * - use own groups (and regularize)
     * - use own priors
     * - multiple runs
     * - what to do about multiple charge states or modded peptides
     * - use add. pep. infos (rt, ms1dev)
     * - add dependencies on peptides in same feature and psms to same peptide (so that there is competition)
     * - ...
     */

/* TODO not yet implemented
 * defaults_.setValue("keep_threshold",
                       "false",
                       "Keep only proteins and protein groups with estimated probability higher than this threshold");
    defaults_.setValue("greedy_group_resolution",
                       "false",
                       "Post-process inference output with greedy resolution of shared peptides based on the parent protein probabilities. Also adds the resolved ambiguity groups to output.");
    defaults_.setValue("combine_indist_groups",
                       "false",
                       "Combine indistinguishable protein groups beforehand to only perform inference on them (probability for the whole group = is ANY of them present).");*/
    defaults_.setValue("annotate_groups_only",
                       "false",
                       "Skips complex inference completely and just annotates indistinguishable groups.");

    defaults_.setValue("top_PSMs",
                       1,
                       "Consider only top X PSMs per spectrum. 0 considers all.");
    defaults_.setMinInt("top_PSMs", 0);


    defaults_.addSection("model_parameters","Model parameters for the Bayesian network");

    defaults_.setValue("model_parameters:prot_prior",
                       0.9,
                       "Protein prior probability ('gamma' parameter).");
    defaults_.setMinFloat("model_parameters:prot_prior", 0.0);
    defaults_.setMaxFloat("model_parameters:prot_prior", 1.0);

    defaults_.setValue("model_parameters:pep_emission",
                       0.1,
                       "Peptide emission probability ('alpha' parameter)");
    defaults_.setMinFloat("model_parameters:pep_emission", 0.0);
    defaults_.setMaxFloat("model_parameters:pep_emission", 1.0);

    defaults_.setValue("model_parameters:pep_spurious_emission",
                       0.001,
                       "Spurious peptide identification probability ('beta' parameter). Usually much smaller than emission from proteins");
    defaults_.setMinFloat("model_parameters:pep_spurious_emission", 0.0);
    defaults_.setMaxFloat("model_parameters:pep_spurious_emission", 1.0);


    defaults_.addSection("loopy_belief_propagation","Settings for the loopy belief propagation algorithm.");

    defaults_.setValue("loopy_belief_propagation:scheduling_type",
                       "priority",
                       "How to pick the next message:"
                           " priority = based on difference to last message (higher = more important)."
                           " fifo = first in first out."
                           " random_spanning_tree = message passing follows a random spanning tree in each iteration");
    defaults_.setValidStrings("loopy_belief_propagation:scheduling_type", {"priority","fifo","random_spanning_tree"});

    //TODO not yet implemented
/*    defaults_.setValue("loopy_belief_propagation:message_difference",
                       "MSE",
                       "How to calculate the difference of distributions in updated messages.");
    defaults_.setValidStrings("loopy_belief_propagation:message_difference", {"MSE"});*/
    defaults_.setValue("loopy_belief_propagation:convergence_threshold",
                       1e-5,
                       "Under which threshold is a message considered to be converged.");
    defaults_.setValue("loopy_belief_propagation:dampening_lambda",
                       1e-3,
                       "How strongly should messages be updated in each step. "
                           "0 = new message overwrites old completely (no dampening),"
                           "1 = old message stays (no convergence, don't do that)"
                           "In-between it will be a convex combination of both. Prevents oscillations but hinders convergence");
    defaults_.setValue("loopy_belief_propagation:max_nr_iterations",
                       1ul<<32,
                       "If not all messages converge, how many iterations should be done at max?");

    defaults_.addSection("param_optimize","Settings for the parameter optimization.");
    defaults_.setValue("param_optimize:aucweight",
                       0.2,
                       "How important is AUC vs calibration of the posteriors?"
                       " 0 = maximize calibration only,"
                       " 1 = maximize AUC only,"
                       " between = convex combination.");
    defaults_.setMinFloat("param_optimize:aucweight", 0.0);
    defaults_.setMaxFloat("param_optimize:aucweight", 1.0);


    // write defaults into Param object param_
    defaultsToParam_();
    updateMembers_();
  }


/* TODO under construction
  void BayesianProteinInferenceAlgorithm::inferPosteriorProbabilities(
      ConsensusMap cmap,
      const ExperimentalDesign& expDesign,
      const String& db,
      const ProteaseDigestion pd,
      ProteinIdentification& proteinIds
      )
  {
    proteinIds.getHits().clear();

    FASTAContainer<TFI_File> fastaDB(db);
    //assume that fasta is given for now. (you could use protein ids in the input idXMLs instead and merge)
    //run peptideindexer (with integrated missing peptide counting or after?)
    PeptideIndexing pi{};
    Param piparams = pi.getDefaults();
    piparams.setValue("annotate_nr_theoretical_peptides", true);
    piparams.setValue("enzyme:name", pd.getEnzymeName());
    //TODO expose enzyme specificity. Default Full
    //piparams.setValue("enzyme:specificity", full)
    pi.setParameters(piparams);
    std::vector<ProteinIdentification> singletonProteinId{proteinIds};

    //Next line: PepIdxr needs to accept ConsensusMap and ExpDesign and annotate one ProtID per sample (frac group/rep combo)
    //pi.run<FASTAContainer<TFI_File>>(fastaDB, singletonProteinId, pepIdConcatReplicates);


    //TODO would be better if we set this after inference but only here we currently have
    // non-const access.
    proteinIds.setScoreType("Posterior Probability");
    proteinIds.setHigherScoreBetter(true);

    // init empty graph
    IDBoostGraph ibg(proteinIds, pepIdConcatReplicates);
    ibg.buildGraph(param_.getValue("nr_PSMs").toBool());
    ibg.computeConnectedComponents();
    ibg.clusterIndistProteinsAndPeptides();

    //TODO how to perform group inference
    // Three options:
    // -collapse proteins to groups beforehand and run inference
    // -use the automatically created indist. groups and report their posterior
    // -calculate prior from proteins for the group beforehand and remove proteins from network (saves computation
    //  because messages are not passed from prots to groups anymore.


    //TODO Use gold search that goes deeper into the grid where it finds the best value.
    //We have to do it on a whole dataset basis though (all CCs). -> I have to refactor to actually store as much
    //as possible (it would be cool to store the inference graph but this is probably not possible bc that is why
    //I split up in CCs.
    // OR I could save the outputs! One value for every protein, per parameter set.

    vector<double> gamma_search{0.5};
    vector<double> beta_search{0.001};
    vector<double> alpha_search{0.1, 0.3, 0.5, 0.7, 0.9};
    //Percolator settings
    //vector<double> alpha_search{0.008, 0.032, 0.128};

    GridSearch<double,double,double> gs{alpha_search, beta_search, gamma_search};

    std::array<size_t, 3> bestParams{};
    //TODO run grid search on reduced graph?
    //TODO if not, think about storing results temporary and only keep the best in the end
    gs.evaluate(GridSearchEvaluator(param_, ibg, proteinIds), -1.0, bestParams);

    std::cout << "Best params found at " << bestParams[0] << "," << bestParams[1] << "," << bestParams[2] << std::endl;

    //TODO write graphfile?
    //TODO let user modify Grid for GridSearch and/or provide some more default settings
  }*/

/*    void BayesianProteinInferenceAlgorithm::inferPosteriorProbabilities(std::vector<ProteinIdentification>& proteinIDs, std::vector<PeptideIdentification>& peptideIDs, OpenMS::ExperimentalDesign expDesign)
  {
    // get enzyme settings from peptideID
    const DigestionEnzymeProtein enzyme = proteinIDs[0].getSearchParameters().digestion_enzyme;
    Size missed_cleavages = proteinIDs[0].getSearchParameters().missed_cleavages;
    EnzymaticDigestion ed{};
    ed.setEnzyme(&enzyme);
    ed.setMissedCleavages(missed_cleavages);

    std::vector<StringView> tempDigests{};
    // if not annotated, assign max nr of digests
    for (auto& protein : proteinIDs[0].getHits())
    {
      // check for existing max nr peptides metavalue annotation
      if (!protein.metaValueExists("maxNrTheoreticalDigests"))
      {
        if(!protein.getSequence().empty())
        {
          tempDigests.clear();
          //TODO check which peptide lengths we should support. Parameter?
          ed.digestUnmodified(protein.getSequence(), tempDigests);
          //TODO add the discarded digestions products, too?
          protein.setMetaValue("maxNrTheoreticalDigests", tempDigests.size());
        }
        else
        {
          //TODO Exception
          std::cerr << "Protein sequence not annotated" << std::endl;
        }
      }
    }

    //TODO would be better if we set this after inference but only here we currently have
    // non-const access.
    proteinIDs[0].setScoreType("Posterior Probability");
    proteinIDs[0].setHigherScoreBetter(true);

    // init empty graph
    IDBoostGraph ibg(proteinIDs[0], peptideIDs);
    ibg.buildGraph(param_.getValue("nr_PSMs").toInt());
    ibg.computeConnectedComponents();
    ibg.clusterIndistProteinsAndPeptides();

    //TODO how to perform group inference
    // Three options:
    // -collapse proteins to groups beforehand and run inference
    // -use the automatically created indist. groups and report their posterior
    // -calculate prior from proteins for the group beforehand and remove proteins from network (saves computation
    //  because messages are not passed from prots to groups anymore.


    //TODO Use gold search that goes deeper into the grid where it finds the best value.
    //We have to do it on a whole dataset basis though (all CCs). -> I have to refactor to actually store as much
    //as possible (it would be cool to store the inference graph but this is probably not possible bc that is why
    //I split up in CCs.
    // OR I could save the outputs! One value for every protein, per parameter set.

    vector<double> gamma_search{0.5};
    vector<double> beta_search{0.001};
    vector<double> alpha_search{0.1, 0.3, 0.5, 0.7, 0.9};
    //Percolator settings
    //vector<double> alpha_search{0.008, 0.032, 0.128};

    GridSearch<double,double,double> gs{alpha_search, beta_search, gamma_search};

    std::array<size_t, 3> bestParams{};
    //TODO run grid search on reduced graph?
    //TODO if not, think about storing results temporary and only keep the best in the end
    gs.evaluate(GridSearchEvaluator(param_, ibg, proteinIDs[0]), -1.0, bestParams);

    std::cout << "Best params found at " << bestParams[0] << "," << bestParams[1] << "," << bestParams[2] << std::endl;

    //TODO write graphfile?
    //TODO let user modify Grid for GridSearch and/or provide some more default settings
  }*/


  void BayesianProteinInferenceAlgorithm::inferPosteriorProbabilities(std::vector<ProteinIdentification>& proteinIDs, std::vector<PeptideIdentification>& peptideIDs)
  {

    //TODO think about how to include missing peptides
    /*
    // get enzyme settings from peptideID
    const DigestionEnzymeProtein enzyme = proteinIDs[0].getSearchParameters().digestion_enzyme;
    Size missed_cleavages = proteinIDs[0].getSearchParameters().missed_cleavages;
    EnzymaticDigestion ed{};
    ed.setEnzyme(&enzyme);
    ed.setMissedCleavages(missed_cleavages);

    std::vector<StringView> tempDigests{};
    // if not annotated, assign max nr of digests
    for (auto& protein : proteinIDs[0].getHits())
    {
      // check for existing max nr peptides metavalue annotation
      if (!protein.metaValueExists("missingTheorDigests"))
      {
        if(!protein.getSequence().empty())
        {
          tempDigests.clear();
          //TODO check which peptide lengths we should support. Parameter?
          //Size nrDiscarded =
          ed.digestUnmodified(protein.getSequence(), tempDigests);
          //TODO add the discarded digestions products, too?
          protein.setMetaValue("missingTheorDigests", tempDigests.size());
        }
        else
        {
          //TODO Exception
          std::cerr << "Protein sequence not annotated" << std::endl;
        }
      }
    }*/

    //TODO would be better if we set this after inference but only here we currently have
    // non-const access.
    proteinIDs[0].setScoreType("Posterior Probability");
    proteinIDs[0].setHigherScoreBetter(true);

    // init empty graph
    IDBoostGraph ibg(proteinIDs[0], peptideIDs);

    bool oldway = false;
    if (oldway)
    {
      //TODO make run info parameter
      ibg.buildGraph(param_.getValue("top_PSMs"));
      ibg.computeConnectedComponents();
      ibg.clusterIndistProteinsAndPeptides();

      //TODO how to perform group inference
      // Three options:
      // -collapse proteins to groups beforehand and run inference
      // -use the automatically created indist. groups and report their posterior
      // -calculate prior from proteins for the group beforehand and remove proteins from network (saves computation
      //  because messages are not passed from prots to groups anymore.


      //TODO Use gold search that goes deeper into the grid where it finds the best value.
      //We have to do it on a whole dataset basis though (all CCs). -> I have to refactor to actually store as much
      //as possible (it would be cool to store the inference graph but this is probably not possible bc that is why
      //I split up in CCs.
      // OR you could save the outputs! One value for every protein, per parameter set.

      vector<double> gamma_search{0.5};
      vector<double> beta_search{0.001};
      vector<double> alpha_search{0.1, 0.3, 0.5, 0.7, 0.9};
      //Percolator settings
      //vector<double> alpha_search{0.008, 0.032, 0.128};

      GridSearch<double,double,double> gs{alpha_search, beta_search, gamma_search};

      std::array<size_t, 3> bestParams{{0, 0, 0}};
      //TODO run grid search on reduced graph?
      //TODO if not, think about storing results temporary (file? mem?) and only keep the best in the end
      gs.evaluate(GridSearchEvaluator(param_, ibg, proteinIDs[0]), -1.0, bestParams);

      std::cout << "Best params found at " << bestParams[0] << "," << bestParams[1] << "," << bestParams[2] << std::endl;
      double bestGamma = gamma_search[bestParams[0]];
      double bestBeta = beta_search[bestParams[1]];
      double bestAlpha = alpha_search[bestParams[2]];
      std::cout << "Running with best parameters again." << std::endl;
      param_.setValue("model_parameters:prot_prior", bestGamma);
      param_.setValue("model_parameters:pep_emission", bestAlpha);
      param_.setValue("model_parameters:pep_spurious_emission", bestBeta);
      ibg.applyFunctorOnCCs(GraphInferenceFunctor(const_cast<const Param&>(param_)));
      ibg.applyFunctorOnCCs(AnnotateIndistGroupsFunctor());
    }
    else
    {
      //TODO make run info parameter
      ibg.buildGraphWithRunInfo(param_.getValue("top_PSMs"));
      ibg.computeConnectedComponents();
      ibg.clusterIndistProteinsAndPeptidesAndExtendGraph();

      vector<double> gamma_search{0.5};
      vector<double> beta_search{0.001};
      vector<double> alpha_search{0.1, 0.3, 0.5, 0.7, 0.9};

      GridSearch<double,double,double> gs{alpha_search, beta_search, gamma_search};

      std::array<size_t, 3> bestParams{{0, 0, 0}};
      //TODO run grid search on reduced graph?
      //TODO if not, think about storing results temporary (file? mem?) and only keep the best in the end
      gs.evaluate(GridSearchEvaluator(param_, ibg, proteinIDs[0]), -1.0, bestParams);

      std::cout << "Best params found at " << bestParams[0] << "," << bestParams[1] << "," << bestParams[2] << std::endl;
      double bestGamma = gamma_search[bestParams[0]];
      double bestBeta = beta_search[bestParams[1]];
      double bestAlpha = alpha_search[bestParams[2]];
      std::cout << "Running with best parameters again." << std::endl;
      param_.setValue("model_parameters:prot_prior", bestGamma);
      param_.setValue("model_parameters:pep_emission", bestAlpha);
      param_.setValue("model_parameters:pep_spurious_emission", bestBeta);
      ibg.applyFunctorOnCCs(ExtendedGraphInferenceFunctor(const_cast<const Param&>(param_)));
      ibg.applyFunctorOnCCs(AnnotateIndistGroupsFunctor());
    }


    //TODO write graphfile?
    //TODO let user modify Grid for GridSearch and/or provide some more default settings
  }
}
