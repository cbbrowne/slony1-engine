/**
 * 
 * This JavaScript class represents a basic slony test.
 * 
 */

function BasicTest(coordinator, results) {
	this.coordinator = coordinator;
	this.testResults = results;
	this.tableIdCounter=1;
	this.sequenceIdCounter=1;
	this.currentOrigin='db1';
}

/**
 * Returns the number of nodes used in the test.
 */
BasicTest.prototype.getNodeCount = function() {
	return 5;
}

/**
 * Returns a slonik preamble. Premables define the node admin conninfo and
 * variables that are used in the rest of the slonik script.
 * 
 * The test framework will substitute variables from the configuration file into
 * the preambles
 * 
 * 
 */
BasicTest.prototype.getSlonikPreamble = function() {
	var nodeCount = this.getNodeCount();
	var slonikPre = 'cluster name=disorder_replica;\n';
	for ( var idx = 1; idx <= nodeCount; idx++) {
		slonikPre += 'node ' + idx + ' admin conninfo=\'dbname=$database.db'
				+ idx + '.dbname host=$database.db' + idx
				+ '.host  '
				+ ' port=$database.db' + idx + '.port'
				+' user=$database.db' + idx				
				+ '.user.slony password=$database.db' + idx + '.password.slony\';\n';
		slonikPre += 'define CONNINFO' + idx + ' \'dbname=$database.db' + idx
				+ '.dbname host=$database.db' + idx + '.host'
				+ ' port=$database.db' + idx + '.port'
				+' user=$database.db'
				+ idx + '.user.slony password=$database.db' + idx
				+ '.password.slony\';\n';
	}
	return slonikPre;
}

/**
 * 
 * Setup a standard replication configuration.
 * 
 * The standard replication configuration is as follows:
 * 
 * 
 * (1)----->(2) \\ \\ (3)------(4) \\ \\ (5)
 * 
 * 
 * Node 1 is the master of set 1. It replicates to the other 4 nodes. Nodes 4
 * and 5 receive there data from node 3 (cascades)
 * 
 * If a configuration is requested with fewer nodes (via the numnodes variable)
 * then nodes with a number higher than numnodes is excluded.
 * 
 * The slonik script generated by this function will also add in the following
 * tables:
 * 
 * 
 * Nothing will be subscribed though.
 */
BasicTest.prototype.setupReplication = function() {

	var result = 0;
	var slonikPre = this.getSlonikPreamble();
	var slonikScript = '';
	for ( var idx = 1; idx <= this.getNodeCount(); idx++) {

		slonikScript += 'try {\n';
		slonikScript += '\tuninstall node(id=' + idx + ');\n';
		slonikScript += '} on error {\n\techo \'slony not installed\';\n}\n';
	}
	// slonikScript += 'sleep(seconds=60);'
	slonikScript += 'init cluster(id=1);\n';

	for ( var idx = 2; idx <= this.getNodeCount(); idx++) {
		slonikScript += 'store node(id=' + idx + ',event node=1);\n';
	}
	/**
	 * Create paths per the diagram
	 */
	// 1->2
	slonikScript += 'store path(server=1,client=2,conninfo=@CONNINFO1 );\n';
	slonikScript += 'store path(server=2,client=1,conninfo=@CONNINFO2 );\n';
	// 1->3
	if (this.getNodeCount() > 2) {
		slonikScript += 'store path(server=1,client=3,conninfo=@CONNINFO1 );\n';
		slonikScript += 'store path(server=3,client=1,conninfo=@CONNINFO3 );\n';
	}

	if (this.getNodeCount() > 3) {
		// 3->4
		slonikScript += 'store path(server=3,client=4,conninfo=@CONNINFO3 );\n';
		slonikScript += 'store path(server=4,client=3,conninfo=@CONNINFO4 );\n';
	}

	if (this.getNodeCount() > 4) {
		// 3->5
		slonikScript += 'store path(server=3,client=5,conninfo=@CONNINFO3 );\n';
		slonikScript += 'store path(server=5,client=3,conninfo=@CONNINFO5 );\n';
	}

	slonikScript += ' create set(id=1,origin=1);\n';
	var thisRef = this;
	var slonik = this.coordinator.createSlonik('init', slonikPre, slonikScript);
	onFinished = {
		onEvent : function(object, event) {
			var result = slonik.getReturnCode();
			thisRef.testResults.assertCheck("slonik - creating nodes+paths+sets", result, 0);
			if (result != 0) {
				thisRef.coordinator
						.abortTest('slonik failure subscribing the set');
			}
			// thisRef.coordinator.stopProcessing();
		}

	};

	var finishedObserver = new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(
			onFinished);
	this.coordinator
			.registerObserver(
					slonik,
					Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
					finishedObserver);

	slonik.run();
	this.coordinator.join(slonik);
	this.coordinator
			.removeObserver(
					slonik,
					Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
					finishedObserver);

	return slonik.getReturnCode();
	
	

}

/**
 * Add in the extra paths so we have a complete set of paths between all nodes.
 * (See setupReplication(), this method adds in the paths the setupReplication
 * skips
 * 
 */
BasicTest.prototype.addCompletePaths = function() {
	var slonikPre = this.getSlonikPreamble();
	var slonikScript = '';
	slonikScript += 'store path(server=1,client=4,conninfo=@CONNINFO1 );\n';
	//slonikScript += 'wait for event(origin=4,confirmed=all,wait on=4);\n';
	slonikScript += 'store path(server=4,client=1,conninfo=@CONNINFO4 );\n';
	//slonikScript += 'wait for event(origin=1,confirmed=all,wait on=1);\n';
	slonikScript += 'store path(server=5,client=1,conninfo=@CONNINFO5 );\n';
	//slonikScript += 'wait for event(origin=1,confirmed=all,wait on=1);\n';
	slonikScript += 'store path(server=1,client=5,conninfo=@CONNINFO1 );\n';
	//slonikScript += 'wait for event(origin=5,confirmed=all,wait on=5);\n';
	slonikScript += 'store path(server=2,client=1,conninfo=@CONNINFO2 );\n';
	//slonikScript += 'wait for event(origin=1,confirmed=all,wait on=1);\n';
	slonikScript += 'store path(server=2,client=5,conninfo=@CONNINFO2 );\n';
	slonikScript += 'store path(server=5,client=2,conninfo=@CONNINFO5 );\n';
	//slonikScript += 'wait for event(origin=5,confirmed=all,wait on=5);\n';
	slonikScript += 'store path(server=3,client=2,conninfo=@CONNINFO3 );\n';
	//slonikScript += 'wait for event(origin=2,confirmed=all,wait on=2);\n';
	slonikScript += 'store path(server=2,client=3,conninfo=@CONNINFO2 );\n';
	slonikScript += 'store path(server=3,client=2,conninfo=@CONNINFO3 );\n';
	slonikScript += 'store path(server=2,client=4,conninfo=@CONNINFO2 );\n';
	slonikScript += 'store path(server=4,client=2,conninfo=@CONNINFO4 );\n';
	//slonikScript += 'wait for event(origin=3,confirmed=all,wait on=3);\n';
	var slonik = this.coordinator.createSlonik('add paths', slonikPre, slonikScript);
	slonik.run();
	this.coordinator.join(slonik);
	this.testResults.assertCheck('paths added okay', slonik.getReturnCode(), 0);
	
}

BasicTest.prototype.getAddTableSlonikScript=function() {
	var slonikScript='';
	slonikScript += ' set add table(id=1, set id=1, fully qualified name=\'disorder.do_customer\',origin=1);\n';
	slonikScript += ' set add sequence(id=1, set id=1, fully qualified name=\'disorder.do_customer_c_id_seq\',origin=1);\n';
	
	slonikScript += ' set add table(id=2, set id=1, fully qualified name=\'disorder.do_item\',origin=1);\n';
	slonikScript += ' set add sequence(id=2, set id=1, fully qualified name=\'disorder.do_item_i_id_seq\',origin=1);\n';
	slonikScript += ' set add table(id=3, set id=1, fully qualified name=\'disorder.do_inventory\',origin=1);\n';
		
	slonikScript += ' set add table(id=4, set id=1, fully qualified name=\'disorder.do_restock\',origin=1);\n';
	slonikScript += ' set add sequence(id=4, set id=1, fully qualified name=\'disorder.do_restock_r_id_seq\',origin=1);\n';
	
	slonikScript += ' set add table(id=5, set id=1, fully qualified name=\'disorder.do_order\',origin=1);\n';
	slonikScript += ' set add sequence(id=5, set id=1, fully qualified name=\'disorder.do_order_o_id_seq\',origin=1);\n';
	
	slonikScript += ' set add table(id=6, set id=1, fully qualified name=\'disorder.do_order_line\',origin=1);\n';
	
	
	slonikScript += ' set add table(id=7, set id=1, fully qualified name=\'disorder.do_config\',origin=1);\n';
	
	
	this.tableIdCounter=8;
	this.sequenceIdCounter=7;
	return slonikScript;
}

BasicTest.prototype.addTables = function() {

	var result = 0;
	var slonikPre = this.getSlonikPreamble();
	var slonikScript = '';
	slonikScript=this.getAddTableSlonikScript();
	var thisRef = this;
	var slonik = this.coordinator.createSlonik('init', slonikPre, slonikScript);
	onFinished = {
		onEvent : function(object, event) {
			var result = slonik.getReturnCode();
			thisRef.testResults.assertCheck("slonik - adding tables to set", result, 0);
			if (result != 0) {
				thisRef.coordinator
						.abortTest('slonik failure subscribing the set');
			}
			// thisRef.coordinator.stopProcessing();
		}

	};

	var finishedObserver = new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(
			onFinished);
	this.coordinator
			.registerObserver(
					slonik,
					Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
					finishedObserver);

	slonik.run();
	this.coordinator.join(slonik);
	this.coordinator
			.removeObserver(
					slonik,
					Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
					finishedObserver);

}

/**
 * Create the databases identified by name in the array dbnames dbname is an
 * array of logical database names that map to connection information in the
 * test configuration file.
 * 
 * dbnames might be something like ['db1','db2']
 */
BasicTest.prototype.prepareDb = function(dbnames) {
	var childList=[];
	this.createDb(dbnames);
	// var schemasql = this.coordinator.readFile("util/sql/pgbench_schema.sql");
	var schemasql = this.coordinator.readFile("disorder/sql/disorder-1.sql");
	for ( var idx = 0; idx < dbnames.length; idx++) {

		psql = this.coordinator.createPsqlCommand(dbnames[idx], schemasql);
		psql.run();
		childList[idx] = psql;
	}
	for ( var idx = 0; idx < dbnames.length; idx++) {
		this.coordinator.join(childList[idx]);
	}
}

BasicTest.prototype.createDb=function(dbnames) {
	var childList = [];
	var thisRef = this;
	for ( var idx = 0; idx < dbnames.length; idx++) {
		db = dbnames[idx];
		createDb = this.coordinator.createCreateDb(db);
		createDbCheck = {
			onEvent : function(source, eventType) {
				thisRef.testResults.assertCheck('createdb is okay', source
						.getReturnCode(), 0);
				if (source.getReturnCode() != 0) {
					thisRef.coordinator.abortTest('createdb failed');
				}
			}
		};

		this.coordinator
				.registerObserver(
						createDb,
						Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
						new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(
								createDbCheck));
		createDb.run();
		childList[idx] = createDb;
	}

	for ( var idx = 0; idx < dbnames.length; idx++) {
		this.coordinator.join(childList[idx]);
	}
	
	
}

BasicTest.prototype.postSeedSetup=function(dbnamelist) {
	var schemasql = this.coordinator.readFile("disorder/sql/disorder-2.sql");
	var psqlArray=[];
	for(var idx=0; idx < dbnamelist.length; idx++) {
		psql = this.coordinator.createPsqlCommand(dbnamelist[idx], schemasql);
		psql.run();
		psqlArray[idx]=psql;
	
	}
	for(var idx=0; idx < dbnamelist.length; idx++) {
		this.coordinator.join(psqlArray[idx]);
	}
	
}

/**
 * Drop the databases identified by name in the array dbnames dbname is an array
 * of logical database names that map to connection information in the test
 * configuration file.
 * 
 * dbnames might be something like ['db1','db2']
 */
BasicTest.prototype.dropDb = function(dbnames) {
	var childList = [];
	var thisRef = this;
	for ( var idx = 0; idx < dbnames.length; idx++) {
		db = dbnames[idx];
		dropDb = this.coordinator.createDropDbCommand(db);
		dropDbCheck = {
			onEvent : function(source, eventType) {
				thisRef.testResults.assertCheck('dropdb is okay', source
						.getReturnCode(), 0);
				if (source.getReturnCode() != 0) {
					thisRef.coordinator.abortTest('dropdb failed');
				}
			}
		};

		this.coordinator
				.registerObserver(
						createDb,
						Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
						new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(
								dropDbCheck));
		dropDb.run();
		childList[idx] = dropDb;
	}

	for ( var idx = 0; idx < dbnames.length; idx++) {
		this.coordinator.join(childList[idx]);
	}

}

/**
 * Returns the amount of time (in seconds) that the test should wait for a
 * slonik sync request to finish and be confirmed.
 * 
 * 
 */
BasicTest.prototype.getSyncWaitTime = function() {
	return 60;
}

/**
 * 
 */
BasicTest.prototype.slonikSync = function(setid, originid) {
	var slonikPre = this.getSlonikPreamble();
	var slonikScript = '';
	slonikScript += ' sync(id=' + originid + ');\n';
	slonikScript += ' wait for event(origin=' + originid + ', wait on='
			+ originid + ',confirmed=all,timeout=' + this.getSyncWaitTime() +');\n';
	var slonik = this.coordinator.createSlonik('sync', slonikPre, slonikScript);
	var thisRef = this;

	var onFinished = {
		onEvent : function(object, event) {
			var result = slonik.getReturnCode();
			thisRef.testResults.assertCheck("slonik completed on success",
					result, 0);
			// thisRef.coordinator.stopProcessing();
		}

	};

	var onTimeout = {
		onEvent : function(object, event) {
			// The sync timed out.
			thisRef.testResults.assertCheck(
					"sync did not finish in the timelimit", true, false);
			slonik.stop();

		}
	};
	var finishedObserver = new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(
			onFinished);
	this.coordinator
			.registerObserver(
					slonik,
					Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
					finishedObserver);

	var timeoutObserver = new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(
			onTimeout);
	var timer = coordinator.addTimerTask('sync failed', this.getSyncWaitTime(),
			timeoutObserver);

	slonik.run();
	this.coordinator.join(slonik);
	this.coordinator
			.removeObserver(
					timer,
					Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_TIMER,
					timeoutObserver);
	this.coordinator
			.removeObserver(
					slonik,
					Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
					finishedObserver);
		
	return slonik.getReturnCode();
}
/**
 * Moves a set (setid) from the origin node to the destination node.
 */
BasicTest.prototype.moveSet = function(setid, origin_node, destination_node) {

	var preamble = this.getSlonikPreamble();
	var slonikScript = 'lock set(id=' + setid + ',origin=' + origin_node
			+ ');\n' + 'move set(id=' + setid + ',old origin=' + origin_node
			+ ', new origin=' + destination_node + ');\n'
			+ 'wait for event(wait on=' + origin_node + ', origin='
			+ origin_node + ', confirmed=all);\n';
	slonik = this.coordinator.createSlonik('moveset', preamble, slonikScript);
	slonik.run();
	this.coordinator.join(slonik);
	this.testResults.assertCheck('move set succeeded', slonik.getReturnCode(),
			0);
	return slonik.getReturnCode();
}

/**
 * Subscribe to a bunch of sets in the background.
 * 
 * This function returns an array of SlonikScript objects setup to subscribe the
 * nodes but these processes have not yet been started but an event listener to
 * validate that the slonik finishes succesfully is registered.
 * 
 * setid=> The set id to subscribe 
 * origin_node=> The origin of the set
 * provier_node=>The provider node
 * subscriber_nodes=> An array of set node ids to be subscribers.
 * 
 */
BasicTest.prototype.subscribeSetBackground = function(setid, origin_node,
		provider_node,
		subscriber_nodes) {
	var slonikScript = '';
	var preamble = this.getSlonikPreamble();
	var slonikList = [];

	for ( var idx = 0; idx < subscriber_nodes.length; idx++) {
		var subscriber_node = subscriber_nodes[idx];
		slonikScript = ' echo \'subscribing to set\';\n';
		slonikScript += ' subscribe set(id=' + setid + ', provider='
				+ provider_node + ', receiver=' + subscriber_node
				+ ', forward=yes);\n';
		//slonikScript += this.generateSlonikWait(origin_node);
		//slonikScript += ' echo \'syncing\';\n';
		//slonikScript += ' sync(id=' + provider_node + ');\n';
		//slonikScript += ' echo \'waiting for event\';\n';
		//slonikScript += this.generateSlonikWait(provider_node);
		slonikScript += ' echo \'finished subscribing ' + subscriber_node +'\' ;\n';

		var slonik = this.coordinator.createSlonik('subscribe ', preamble,
				slonikScript);
		slonikList[idx] = slonik;
		var thisRef = this;
		onFinished = {
			onEvent : function(object, event) {
				var result = object.getReturnCode();
				thisRef.testResults
						.assertCheck("slonik - subscribing set", result, 0);
				thisRef.coordinator
						.removeObserver(
								object,
								Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
								this);
				// if(result != 0) {
				// thisRef.coordinator.abortTest('slonik failure subscribing the
				// set');
				// }
				// thisRef.coordinator.stopProcessing();
			}

		};

		this.coordinator
				.registerObserver(
						slonik,
						Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
						new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(
								onFinished));

	}

	return slonikList;
}

BasicTest.prototype.subscribeSet = function(set_id, origin_node,provider_node,
		subscriber_nodes) {
	var slonikList = this.subscribeSetBackground(set_id, origin_node,provider_node,
			subscriber_nodes);
	for ( var idx = 0; idx < slonikList.length; idx++) {
		slonikList[idx].run();
	}
	for ( var idx = 0; idx < slonikList.length; idx++) {
		this.coordinator.join(slonikList[idx]);
	}
}

/**
 * Teardown the slony cluster. This is typically done at the end of a test to
 * leave the databases in a clean state for the next test.
 */
BasicTest.prototype.teardownSlony = function() {
	var slonikPre = this.getSlonikPreamble();
	var slonikScript = '';
	for ( var idx = 1; idx <= this.getNodeCount(); idx++) {

		slonikScript += 'try {\n';
		slonikScript += '\tuninstall node(id=' + idx + ');\n';
		slonikScript += '} on error {\n\techo \'slony not installed\';\n}\n';
	}
	var slonik = this.coordinator.createSlonik("uninstall", slonikPre,
			slonikScript);
	var thisRef = this;
	onFinished = {
		onEvent : function(object, event) {
			var result = slonik.getReturnCode();
			thisRef.testResults.assertCheck("slonik - uninstalling nodes", result, 0);
			if (result != 0) {
				thisRef.coordinator.abortTest('slonik failure uninstalling ');
			}
			// thisRef.coordinator.stopProcessing();
		}

	};

	var finishedObserver = new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(
			onFinished);
	this.coordinator
			.registerObserver(
					slonik,
					Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
					finishedObserver);

	slonik.run();
	this.coordinator.join(slonik);

}



BasicTest.prototype.generateLoad = function(set_origin) {

	var disorderClientJs = this.coordinator.readFile('disorder/client/disorder.js');
	disorderClientJs+= this.coordinator.readFile('disorder/client/run_fixed_load.js');
	var load = this.coordinator.clientScript(disorderClientJs,this.getCurrentOrigin());
	load.run();
	return load;
}

BasicTest.prototype.seedData = function(scaling) {

	this.coordinator.log("Seeding data with scaling " + scaling);
	var populatePsql = this.coordinator.createPsqlCommand('db1',
			'SET SEARCH_PATH=disorder,public; SELECT disorder.populate(' + scaling + ');');
	populatePsql.run();
	return populatePsql;

}

BasicTest.prototype.compareDb=function(lhs_db, rhs_db) {
	//Compare the results.
	this.coordinator.log('comparing' + lhs_db + 'rhs_db');
	var queryList = [
	                 ['SELECT c_id,c_name,c_total_orders,c_total_value FROM disorder.do_customer order by c_id','c_id']
	                 ,['SELECT i_id,i_name,i_price,i_in_production FROM disorder.do_item order by i_id','i_id']
	                 ,['SELECT ii_id, ii_in_stock,ii_reserved,ii_total_sold FROM disorder.do_inventory order by ii_id','ii_id']
	                 ];
	
	compareFinished = {
			onEvent : function(object, event) {			
					compResult=object.getResultCode();
					results.assertCheck("history is equal for " + lhs_db + " vs " + rhs_db,compResult,Packages.info.slony.clustertest.testcoordinator.CompareOperation.COMPARE_EQUALS)
					coordinator.stopProcessing();
					coordinator.removeObserver(object,event,this);
									
					
			}
		};


	
	for(var idx=0; idx < queryList.length; idx++) {
		var compareOp = this.coordinator.createCompareOperation(lhs_db,rhs_db,queryList[idx][0],
				queryList[idx][1]);
		this.coordinator.registerObserver(compareOp, Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_FINISHED,
				new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(compareFinished));

		compareOp.run();
		//At some point all the compare could be done concurrently?
		this.coordinator.join(compareOp);
	}
	

}

BasicTest.prototype.getClusterName = function () {
	return properties.getProperty('clustername');
}


/**
 *  Create a second replication set.
 *  
 *  This replication set will have the node indicated as the origin.
 *  
 *  It will contain the comments table, a table with a fkey to products.
 *  
 */
BasicTest.prototype.createSecondSet=function(origin) {
	var slonikPreamble = this.getSlonikPreamble();
	var slonikScript = 'create set(id=2, origin=' + origin + ',comment=\'second set\');\n'
		+ 'set add table(set id=2,origin='  + origin + ',id=' + this.tableIdCounter 
		+', fully qualified name=\'disorder.do_item_review\');\n' 
		+ 'set add sequence(set id=2, origin=' + origin + ', id=' + this.sequenceIdCounter
		+ ', fully qualified name=\'disorder.do_item_review_ir_id_seq\');\n';
	
	this.tableIdCounter++;
	this.sequenceIdCounter++;
	
	var slonik = this.coordinator.createSlonik('create set 2', slonikPreamble,slonikScript);
	slonik.run();
	this.coordinator.join(slonik);
	this.testResults.assertCheck('create set to succeeded',slonik.getReturnCode(),0);
	
}

/**
 * Measure the lag that a particular node is experiencing.
 * 
 */
BasicTest.prototype.measureLag = function(event_node, lag_node) {
	var connection = this.coordinator.createJdbcConnection('db' + event_node);
	var statement = connection.createStatement();
	var rs=undefined;
	try {
		rs = statement.executeQuery('SELECT extract(\'epoch\' from st_lag_time) from _' + this.getClusterName() +
				'.sl_status where st_origin=' + event_node + ' AND st_received=' + lag_node);
		rs.next();
		return rs.getInt(1);
	}
	catch(e) {
		this.testResults.assertCheck('error checking lag on ' + event_node + ':'+e,true,false);
		return -1;
	}
	finally {
		if(rs != undefined) {
			rs.close();
		}
		statement.close();
		connection.close();
	}
}

BasicTest.prototype.startDataChecks=function(node_id) {
	var disorderClientJs = this.coordinator.readFile('disorder/client/disorder.js');
	disorderClientJs+= this.coordinator.readFile('disorder/client/run_check_load.js');
	var load = this.coordinator.clientScript(disorderClientJs,'db' + node_id);
	var thisRef = this;
	var failOnError = {
		onEvent : function(object,event) {
			thisRef.testResults.assertCheck("the data check script reported an error",true,false);
		}
	};
	this.coordinator.registerObserver(load,Packages.info.slony.clustertest.testcoordinator.Coordinator.EVENT_ERROR,
			new Packages.info.slony.clustertest.testcoordinator.script.ExecutionObserver(failOnError));
	load.run();
	return load;

}

/**
 * Verifies that the node is read only.
 * We do this by attempting to INSERT into the do_config table and
 * making sure that the insert fails.
 */
BasicTest.prototype.verifyReadOnly=function(node_id) {
	this.coordinator.log('verifying read only status of node ' + node_id);
	var connection=this.coordinator.createJdbcConnection('db' + node_id);
	var stat = connection.createStatement();
	try {
		var result = stat.execute("INSERT INTO disorder.do_config(cfg_opt,cfg_val) VALUES ('test1','test2');");
		this.testResults.assertCheck(node_id + 'is read only',result,false);
	}
	catch(error) {
		this.testResults.assertCheck(node_id + ' is read only',true,true);
		
	}
	finally {
		stat.close();
		connection.close();
	}
	


}
BasicTest.prototype.getCurrentOrigin=function() {
	return this.currentOrigin;
}

/**
 * Returns a string consisting of the slonik commands to 
 * WAIT for an event at the event_node to be confirmed by all other
 * nodes.
 * 
 * The default version of this command does a confirmed=all, but subclasses
 * can overwrite then when 'all' is not the behaviour they want.
 * 
 */
BasicTest.prototype.generateSlonikWait=function(event_node) {
	var slonikScript = ' wait for event(origin=' + event_node + ', wait on='
		+ event_node + ',confirmed=all);\n';
	return slonikScript;
}


BasicTest.prototype.getSlonConfFileMap=function(event_node) {
    var map = Packages.java.util.HashMap();
    map.put('cluster_name',this.getClusterName());   
    return map;
}
