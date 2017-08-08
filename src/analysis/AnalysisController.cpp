#include "AnalysisController.hh"

const map<string, DataFlowAnalysisType> DataFlowAnalysisTypeMap = { { "ifds_uninit", DataFlowAnalysisType::IFDS_UninitializedVariables },
																																	{ "ifds_taint", DataFlowAnalysisType::IFDS_TaintAnalysis },
																																	{ "ifds_type", DataFlowAnalysisType::IFDS_TypeAnalysis },
																																	{ "ide_taint", DataFlowAnalysisType::IDE_TaintAnalysis },
																																	{ "ifds_solvertest", DataFlowAnalysisType::IFDS_SolverTest },
																																	{ "ide_solvertest", DataFlowAnalysisType::IDE_SolverTest },
																																	{ "mono_intra_fullconstpropagation", DataFlowAnalysisType::MONO_Intra_FullConstantPropagation },
																																	{ "mono_intra_solvertest",DataFlowAnalysisType::MONO_Intra_SolverTest },
																																	{ "mono_inter_solvertest",DataFlowAnalysisType::MONO_Inter_SolverTest },
																																	{ "none", DataFlowAnalysisType::None } };

ostream& operator<<(ostream& os, const DataFlowAnalysisType& D) {
	static const array<string, 10> str {{
		"AnalysisType::IFDS_UninitializedVariables",
		"AnalysisType::IFDS_TaintAnalysis",
		"AnalysisType::IDE_TaintAnalysis",
		"AnalysisType::IFDS_TypeAnalysis",
		"AnalysisType::IFDS_SolverTest",
		"AnalysisType::IDE_SolverTest",
		"AnalysisType::MONO_Intra_FullConstantPropagation",
		"AnalysisType::MONO_Intra_SolverTest",
		"AnalysisType::MONO_Inter_SoverTest",
		"AnalysisType::None",
	}};
	return os << str.at(static_cast<underlying_type_t<DataFlowAnalysisType>>(D));
}

const map<string, ExportType> ExportTypeMap = { { "json" , ExportType::JSON } };

ostream& operator<<(ostream& os, const ExportType& E) {
	static const array<string, 1> str {{
		"ExportType::JSON"
	}};
	return os << str.at(static_cast<underlying_type_t<ExportType>>(E));
}

  AnalysisController::AnalysisController(ProjectIRCompiledDB& IRDB,
                     vector<DataFlowAnalysisType> Analyses, bool WPA_MODE, bool Mem2Reg_MODE,
					 bool PrintEdgeRecorder) {
		auto& lg = lg::get();
		BOOST_LOG_SEV(lg, INFO) << "Constructed the analysis controller.";
		BOOST_LOG_SEV(lg, INFO) << "Found the following IR files for this project: ";
    for (auto file : IRDB.source_files) {
      BOOST_LOG_SEV(lg, INFO) << "\t" << file;
    }
    if (WPA_MODE) {
    	 // here we link every llvm module into a single module containing the entire IR
    	BOOST_LOG_SEV(lg, INFO) << "link all llvm modules into a single module for WPA ...\n";
    	IRDB.linkForWPA();
    }
    /*
     * Important
     * ---------
     * Note that if WPA_MODE was chosen by the user, the IRDB only contains one
     * single llvm::Module containing the whole program. For that reason all
     * subsequent loops are no real loops.
     */

    // here we perform a pre-analysis and run some very important passes over
    // all of the IR modules in order to perform various data flow analysis
    BOOST_LOG_SEV(lg, INFO) << "Start pre-analyzing modules.";
    for (auto& module_entry : IRDB.modules) {
      BOOST_LOG_SEV(lg, INFO) << "Pre-analyzing module: " << module_entry.first;
      llvm::Module& M = *(module_entry.second.get());
      llvm::LLVMContext& C = *(IRDB.contexts[module_entry.first].get());
      // TODO Have a look at this stuff from the future at some point in time
      /// PassManagerBuilder - This class is used to set up a standard optimization
      /// sequence for languages like C and C++, allowing some APIs to customize the
      /// pass sequence in various ways. A simple example of using it would be:
      ///
      ///  PassManagerBuilder Builder;
      ///  Builder.OptLevel = 2;
      ///  Builder.populateFunctionPassManager(FPM);
      ///  Builder.populateModulePassManager(MPM);
      ///
      /// In addition to setting up the basic passes, PassManagerBuilder allows
      /// frontends to vend a plugin API, where plugins are allowed to add extensions
      /// to the default pass manager.  They do this by specifying where in the pass
      /// pipeline they want to be added, along with a callback function that adds
      /// the pass(es).  For example, a plugin that wanted to add a loop optimization
      /// could do something like this:
      ///
      /// static void addMyLoopPass(const PMBuilder &Builder, PassManagerBase &PM) {
      ///   if (Builder.getOptLevel() > 2 && Builder.getOptSizeLevel() == 0)
      ///     PM.add(createMyAwesomePass());
      /// }
      ///   ...
      ///   Builder.addExtension(PassManagerBuilder::EP_LoopOptimizerEnd,
      ///                        addMyLoopPass);
      ///   ...
      // But for now, stick to what is well debugged
      llvm::legacy::PassManager PM;
      GeneralStatisticsPass* GSP = new GeneralStatisticsPass();
      ValueAnnotationPass* VAP = new ValueAnnotationPass(C);
      llvm::CFLSteensAAWrapperPass* SteensP = new llvm::CFLSteensAAWrapperPass();
      llvm::AAResultsWrapperPass* AARWP = new llvm::AAResultsWrapperPass();
      if (Mem2Reg_MODE) {
      	llvm::FunctionPass* Mem2Reg = llvm::createPromoteMemoryToRegisterPass();
      	PM.add(Mem2Reg);
      }
      PM.add(GSP);
      PM.add(VAP);
      PM.add(SteensP);
      PM.add(AARWP);
      PM.run(M);
      // just to be sure that none of the passes has messed up the module!
      bool broken_debug_info = false;
      if (llvm::verifyModule(M, &llvm::errs(), &broken_debug_info)) {
        BOOST_LOG_SEV(lg, CRITICAL) << "AnalysisController: module is broken!";
      }
      if (broken_debug_info) {
        BOOST_LOG_SEV(lg, WARNING) << "AnalysisController: debug info is broken.";
      }
      // obtain the very important alias analysis results
      // and construct the intra-procedural points-to graphs
      for (auto& function : M) {
      	IRDB.ptgs.insert(make_pair(function.getName().str(), unique_ptr<PointsToGraph>(new PointsToGraph(AARWP->getAAResults(), &function))));
			}
    }
    BOOST_LOG_SEV(lg, INFO) << "Pre-analysis completed.";
    IRDB.print();

    DBConn& db = DBConn::getInstance();
		db.synchronize(&IRDB);
		auto M = IRDB.getModuleDefiningFunction("main");
		for (auto& F : *M) {
			if (!F.isDeclaration()) {
			//	cout << F.getName().str() << "\n";
			//	db << *IRDB.getPointsToGraph(F.getName().str());
			}
		}

    // db << IRDB;

    // reconstruct the inter-modular class hierarchy and virtual function tables
    BOOST_LOG_SEV(lg, INFO) << "Reconstruct the class hierarchy.";
    LLVMStructTypeHierarchy CH(IRDB);
    BOOST_LOG_SEV(lg, INFO) << "Reconstruction of class hierarchy completed.";
    CH.print();
    CH.printAsDot();

    // db << CH;
    // db >> CH;

    IFDSSpecialSummaries<const llvm::Value*>& specialSummaries =
    		IFDSSpecialSummaries<const llvm::Value*>::getInstance();
    // cout << specialSummaries << endl;

			// check and test the summary generation:
//      			cout << "GENERATE SUMMARY" << endl;
//      			LLVMIFDSSummaryGenerator<LLVMBasedICFG&, IFDSUnitializedVariables>
//      								Generator(M.getFunction("_Z6squarei"), icfg);
//      			auto summary = Generator.generateSummaryFlowFunction();


    /*
     * Perform whole program analysis (WPA) analysis
     * -----------
     */
    if (WPA_MODE) {
   	  // There is only one module left, because we have linked earlier
	  	llvm::Module& M = *IRDB.getWPAModule();
      LLVMBasedICFG ICFG(CH, IRDB, WalkerStrategy::Pointer, ResolveStrategy::OTF, {"main"});
      ICFG.print();
      ICFG.printAsDot("interproc_cfg.dot");
	  // CFG is only needed for intra-procedural monotone framework
      LLVMBasedCFG CFG;
      /*
       * Perform all the analysis that the user has chosen.
       */
      for (DataFlowAnalysisType analysis : Analyses) {
				BOOST_LOG_SEV(lg, INFO) << "Performing analysis: " << analysis;
      	switch (analysis) {
      		case DataFlowAnalysisType::IFDS_TaintAnalysis:
       		{ // caution: observer '{' and '}' we work in another scope
       			IFDSTaintAnalysis taintanalysisproblem(ICFG);
       			LLVMIFDSSolver<const llvm::Value*, LLVMBasedICFG&> llvmtaintsolver(taintanalysisproblem, true);
       			llvmtaintsolver.solve();
       			break;
       		}
       		case DataFlowAnalysisType::IDE_TaintAnalysis:
       		{ // caution: observer '{' and '}' we work in another scope
       			IDETaintAnalysis taintanalysisproblem(ICFG);
       			LLVMIDESolver<const llvm::Value*, const llvm::Value*, LLVMBasedICFG&> llvmtaintsolver(taintanalysisproblem, true);
       			llvmtaintsolver.solve();
       			break;
       		}
       		case DataFlowAnalysisType::IFDS_TypeAnalysis:
       		{ // caution: observer '{' and '}' we work in another scope
       			IFDSTypeAnalysis typeanalysisproblem(ICFG);
       			LLVMIFDSSolver<const llvm::Value*, LLVMBasedICFG&> llvmtypesolver(typeanalysisproblem, true);
       			llvmtypesolver.solve();
       			break;
       		}
       		case DataFlowAnalysisType::IFDS_UninitializedVariables:
       		{ // caution: observer '{' and '}' we work in another scope
       			IFDSUnitializedVariables uninitializedvarproblem(ICFG);
       			LLVMIFDSSolver<const llvm::Value*, LLVMBasedICFG&> llvmunivsolver(uninitializedvarproblem, true);
						llvmunivsolver.solve();
						llvmunivsolver.exportJSONDataModel();
						// if (PrintEdgeRecorder) {
						// 	llvmunivsolver.dumpAllIntraPathEdges();
						// 	llvmunivsolver.dumpAllInterPathEdges();
						// }
       			break;
       		}
       		case DataFlowAnalysisType::IFDS_SolverTest:
       		{
       			IFDSSolverTest ifdstest(ICFG);
       			LLVMIFDSSolver<const llvm::Value*, LLVMBasedICFG&> llvmifdstestsolver(ifdstest, true);
       			llvmifdstestsolver.solve();
       			break;
       		}
       		case DataFlowAnalysisType::IDE_SolverTest:
       		{
       			IDESolverTest idetest(ICFG);
       			LLVMIDESolver<const llvm::Value*, const llvm::Value*, LLVMBasedICFG&> llvmidetestsolver(idetest, true);
       			llvmidetestsolver.solve();
       			break;
       		}
					case DataFlowAnalysisType::MONO_Intra_FullConstantPropagation:
					{
						IntraMonoFullConstantPropagation intra(CFG, IRDB.getFunction("main"));
						LLVMIntraMonotoneSolver<pair<const llvm::Value*, unsigned>, LLVMBasedCFG&> solver(intra, true);
           	solver.solve();
						break;
					}
       		case DataFlowAnalysisType::MONO_Intra_SolverTest:
       		{
           	IntraMonotoneSolverTest intra(CFG, IRDB.getFunction("main"));
           	LLVMIntraMonotoneSolver<const llvm::Value*, LLVMBasedCFG&> solver(intra, true);
           	solver.solve();
       			break;
       		}
					case DataFlowAnalysisType::MONO_Inter_SolverTest:
					{
						InterMonotoneSolverTest inter(ICFG);
						LLVMInterMonotoneSolver<const llvm::Value*, LLVMBasedICFG&> solver(inter, true);
						solver.solve();
						break;
					}
       		case DataFlowAnalysisType::None:
					{
						break;
					}
       		default:
       			BOOST_LOG_SEV(lg, CRITICAL) << "The analysis it not valid";
       			break;
       		}
       }
    }
    /*
     * Perform module-wise (MW) analysis
     */
    else {
    	map<const llvm::Module*, LLVMBasedICFG> MWICFGs;
    	/*
       * We build all the call- and points-to graphs which can be used for
       * all of the analysis of course.
       */
      
			auto& Mod = *IRDB.getModuleDefiningFunction("main");
			auto& Mod_2 = *IRDB.getModuleDefiningFunction("_Z3foov");

			LLVMBasedICFG ICFG(CH, IRDB, Mod, WalkerStrategy::Pointer, ResolveStrategy::OTF);
			ICFG.print();
			ICFG.printAsDot("icfg_main.dot");
			LLVMBasedICFG ICFG_2(CH, IRDB, Mod_2, WalkerStrategy::Pointer, ResolveStrategy::OTF);
			ICFG.print();
			ICFG_2.printAsDot("icfg_foo.dot");

			ICFG.mergeWith(ICFG_2);
			ICFG.printAsDot("icfg_after_merge.dot");

			// for (auto M : IRDB.getAllModules()) {
      //  	LLVMBasedICFG ICFG(CH, IRDB, *M);
      //   	// // store them away for later use
      //   	// MWICFGs.insert(make_pair(M, ICFG));
      // }

       /*
        * Perform all the analysis that the user has chosen.
        */
       for (DataFlowAnalysisType analysis : Analyses) {
				 BOOST_LOG_SEV(lg, INFO) << "Performing analysis: " << analysis;
       	switch (analysis) {
       		case DataFlowAnalysisType::IFDS_TaintAnalysis:
       		{ // caution: observer '{' and '}' we work in another scope
//       	    // Here we create our module-wise result storage that is needed
//       	    // when performing a module-wise analysis.
//       	    ModuleWiseResults<const llvm::Value*> MWR;
//       	   	// prepare the ICFG the data-flow analyses are build on
//       	    cout << "starting the chosen data-flow analyses ...\n";
//       	    for (auto& module_entry : IRDB.modules) {
//       	    	// create the analyses problems queried by the user and start analyzing
//       	    	llvm::Module& M = *(module_entry.second);
//       	    	llvm::LLVMContext& C = *IRDB.getLLVMContext(M.getModuleIdentifier());
//       	    	LLVMBasedICFG icfg(M, CH, IRDB);
//       	    	cout << "call graph:\n";
//       	    	icfg.print();
//       	    	icfg.printAsDot("call_graph.dot");
//
//       	      // Store the information for the analyzed module away and combine them later
//       	      if (!WPA_MODE) {
//       	      	MWR.addModuleAnalysisInfo(M.getModuleIdentifier());
//       	      }
//       	    }
//       			IFDSTaintAnalysis taintanalysisproblem(icfg);
//       			LLVMIFDSSolver<const llvm::Value*, LLVMBasedICFG&> llvmtaintsolver(taintanalysisproblem, true);
//       			llvmtaintsolver.solve();
       			break;
       		}
       		case DataFlowAnalysisType::IDE_TaintAnalysis:
       		{ // caution: observer '{' and '}' we work in another scope
//       	    // Here we create our module-wise result storage that is needed
//       	    // when performing a module-wise analysis.
//       	    ModuleWiseResults<const llvm::Value*> MWR;
//       	   	// prepare the ICFG the data-flow analyses are build on
//       	    cout << "starting the chosen data-flow analyses ...\n";
//       	    for (auto& module_entry : IRDB.modules) {
//       	    	// create the analyses problems queried by the user and start analyzing
//       	    	llvm::Module& M = *(module_entry.second);
//       	    	llvm::LLVMContext& C = *IRDB.getLLVMContext(M.getModuleIdentifier());
//       	    	LLVMBasedICFG icfg(M, CH, IRDB);
//       	    	cout << "call graph:\n";
//       	    	icfg.print();
//       	    	icfg.printAsDot("call_graph.dot");
//
//       	      // Store the information for the analyzed module away and combine them later
//       	      if (!WPA_MODE) {
//       	      	MWR.addModuleAnalysisInfo(M.getModuleIdentifier());
//       	      }
//       	    }
//       			//IDETaintAnalysis taintanalysisproblem(icfg);
//       			//LLVMIDESolver<const llvm::Value*, const llvm::Value*, LLVMBasedICFG&> llvmtaintsolver(taintanalysisproblem, true);
//       			//llvmtaintsolver.solve();
       			break;
       		}
       		case DataFlowAnalysisType::IFDS_TypeAnalysis:
       		{ // caution: observer '{' and '}' we work in another scope
//       	    // Here we create our module-wise result storage that is needed
//       	    // when performing a module-wise analysis.
//       	    ModuleWiseResults<const llvm::Value*> MWR;
//       	   	// prepare the ICFG the data-flow analyses are build on
//       	    cout << "starting the chosen data-flow analyses ...\n";
//       	    for (auto& module_entry : IRDB.modules) {
//       	    	// create the analyses problems queried by the user and start analyzing
//       	    	llvm::Module& M = *(module_entry.second);
//       	    	llvm::LLVMContext& C = *IRDB.getLLVMContext(M.getModuleIdentifier());
//       	    	LLVMBasedICFG icfg(M, CH, IRDB);
//       	    	cout << "call graph:\n";
//       	    	icfg.print();
//       	    	icfg.printAsDot("call_graph.dot");
//
//       	      // Store the information for the analyzed module away and combine them later
//       	      if (!WPA_MODE) {
//       	      	MWR.addModuleAnalysisInfo(M.getModuleIdentifier());
//       	      }
//       	    }
//       			IFDSTypeAnalysis typeanalysisproblem(icfg);
//       			LLVMIFDSSolver<const llvm::Value*, LLVMBasedICFG&> llvmtypesolver(typeanalysisproblem, true);
//       			llvmtypesolver.solve();
       			break;
       		}
       		case DataFlowAnalysisType::IFDS_UninitializedVariables:
       		{ // caution: observer '{' and '}' we work in another scope
//       	    // Here we create our module-wise result storage that is needed
//       	    // when performing a module-wise analysis.
//       	    ModuleWiseResults<const llvm::Value*> MWR;
//       	   	// prepare the ICFG the data-flow analyses are build on
//       	    cout << "starting the chosen data-flow analyses ...\n";
//       	    for (auto& module_entry : IRDB.modules) {
//       	    	// create the analyses problems queried by the user and start analyzing
//       	    	llvm::Module& M = *(module_entry.second);
//       	    	llvm::LLVMContext& C = *IRDB.getLLVMContext(M.getModuleIdentifier());
//       	    	LLVMBasedICFG icfg(M, CH, IRDB);
//       	    	cout << "call graph:\n";
//       	    	icfg.print();
//       	    	icfg.printAsDot("call_graph.dot");
//
//       	      // Store the information for the analyzed module away and combine them later
//       	      if (!WPA_MODE) {
//       	      	MWR.addModuleAnalysisInfo(M.getModuleIdentifier());
//       	      }
//       	    }
//       			IFDSUnitializedVariables uninitializedvarproblem(icfg);
//       			LLVMIFDSSolver<const llvm::Value*, LLVMBasedICFG&> llvmunivsolver(uninitializedvarproblem, true);
//       			llvmunivsolver.solve();
//
//       			// check and test the summary generation:
//   //      			cout << "GENERATE SUMMARY" << endl;
//   //      			LLVMIFDSSummaryGenerator<LLVMBasedICFG&, IFDSUnitializedVariables>
//   //      								Generator(M.getFunction("_Z6squarei"), icfg);
//   //      			auto summary = Generator.generateSummaryFlowFunction();
       			break;
       		}
       		case DataFlowAnalysisType::IFDS_SolverTest:
       		{
//       			map<const llvm::Module*, IFDSSummaryPool<const llvm::Value*>> MWIFDSSummaryPools;
//       	    for (auto M : IRDB.getAllModules()) {
//       	    	IFDSSolverTest ifdstest(ICFG);
//       	    	LLVMIFDSSolver<const llvm::Value*, LLVMBasedICFG&> llvmifdstestsolver(ifdstest, true);
//       	    	llvmifdstestsolver.solve();
       	    	break;
       		}
       		case DataFlowAnalysisType::IDE_SolverTest:
       		{
//       	    // Here we create our module-wise result storage that is needed
//       	    // when performing a module-wise analysis.
//       	    ModuleWiseResults<const llvm::Value*> MWR;
//       	   	// prepare the ICFG the data-flow analyses are build on
//       	    cout << "starting the chosen data-flow analyses ...\n";
//       	    for (auto& module_entry : IRDB.modules) {
//       	    	// create the analyses problems queried by the user and start analyzing
//       	    	llvm::Module& M = *(module_entry.second);
//       	    	llvm::LLVMContext& C = *IRDB.getLLVMContext(M.getModuleIdentifier());
//       	    	LLVMBasedICFG icfg(M, CH, IRDB);
//       	    	cout << "call graph:\n";
//       	    	icfg.print();
//       	    	icfg.printAsDot("call_graph.dot");
//
//       	      // Store the information for the analyzed module away and combine them later
//       	      if (!WPA_MODE) {
//       	      	MWR.addModuleAnalysisInfo(M.getModuleIdentifier());
//       	      }
//       	    }
//       			//IDESolverTest idetest(icfg);
//       			//LLVMIDESolver<const llvm::Value*, const llvm::Value*, LLVMBasedICFG&> llvmidetestsolver(idetest, true);
//       			//llvmidetestsolver.solve();
       			break;
       		}
       		case DataFlowAnalysisType::MONO_Intra_SolverTest:
       		{
//       			LLVMBasedCFG cfg;
//           	MonotoneSolverTest intra(cfg, IRDB.getFunction("main"));
//           	LLVMMonotoneSolver<const llvm::Value*, LLVMBasedCFG&> solver(intra, true);
//           	solver.solve();
//       			break;
//       		}
//       		case AnalysisType::MONO_Inter_SolverTest:
//       		{
//       			cout << "MONO_Inter_SolverTest\n";
//       	    // Here we create our module-wise result storage that is needed
//       	    // when performing a module-wise analysis.
//       	    ModuleWiseResults<const llvm::Value*> MWR;
//       	   	// prepare the ICFG the data-flow analyses are build on
//       	    cout << "starting the chosen data-flow analyses ...\n";
//       	    for (auto& module_entry : IRDB.modules) {
//       	    	// create the analyses problems queried by the user and start analyzing
//       	    	llvm::Module& M = *(module_entry.second);
//       	    	llvm::LLVMContext& C = *IRDB.getLLVMContext(M.getModuleIdentifier());
//       	    	LLVMBasedICFG icfg(M, CH, IRDB);
//       	    	cout << "call graph:\n";
//       	    	icfg.print();
//       	    	icfg.printAsDot("call_graph.dot");
//
//       	      // Store the information for the analyzed module away and combine them later
//       	      if (!WPA_MODE) {
//       	      	MWR.addModuleAnalysisInfo(M.getModuleIdentifier());
//       	      }
//       	    }
//           	cout << "yet to be implemented!\n";
       			break;
       		}
       		case DataFlowAnalysisType::None:
					{
						// cout << "LLVMBASEDICFG TEST\n";
						// LLVMBasedICFG G(CH, IRDB, *IRDB.getModuleDefiningFunction("main"));
						// cout << "G\n";
						// G.print();
						// G.printAsDot("main.dot");
						// cout << "H\n";
						// LLVMBasedICFG H(CH, IRDB, *IRDB.getModuleDefiningFunction("_Z3foov"));
						// H.print();
						// H.printAsDot("src1.dot");
						// cout << "NOW MERGING\n";
						// G.mergeWith(H);
						// G.print();
						break;
					}
       		default:
       			BOOST_LOG_SEV(lg, CRITICAL) << "The analysis it not valid";
       			break;
       	}
       }
       	// after every module has been analyzed the analyses results must be
        // merged and the final results must be computed
        BOOST_LOG_SEV(lg, INFO) << "Combining module-wise results";
        // start at the main function and iterate over the entire program combining
        // all results!
        llvm::Module& M = *IRDB.getModuleDefiningFunction("main");
        BOOST_LOG_SEV(lg, INFO) << "Combining module-wise results done, computation completed!";
    }
    BOOST_LOG_SEV(lg, INFO) << "Data-flow analyses completed.";
}

