/*
 * casstcl_cassandra - Functions used to talk to Cassandra through the cpp driver
 * 						eg. upsert, select, delete, list keyspace, columns, and tables
 *
 * casstcl - Tcl interface to CassDB
 *
 * Copyright (C) 2014 FlightAware LLC
 *
 * freely redistributable under the Berkeley license
 */

 #include "casstcl_cassandra.h"

/*
 *--------------------------------------------------------------
 *
 * casstcl_cassObjectDelete -- command deletion callback routine.
 *
 * Results:
 *      ...destroys the cass connection handle.
 *      ...frees memory.
 *
 * Side effects:
 *      None.
 *
 *--------------------------------------------------------------
 */
void
casstcl_cassObjectDelete (ClientData clientData)
{
    casstcl_sessionClientData *ct = (casstcl_sessionClientData *)clientData;

    assert (ct->cass_session_magic == CASS_SESSION_MAGIC);

	cass_ssl_free (ct->ssl);
    cass_cluster_free (ct->cluster);
    cass_session_free (ct->session);

    ckfree((char *)clientData);
}

/*
 *----------------------------------------------------------------------
 *
 * casstcl_make_upsert_statement_from_objv --
 *
 *   This takes an objv and objc containing possible arguments such
 *   as -mapunknown, -nocomplain and -ifnotexists and in the objv
 *   always a fully qualified table name and a list of key-value pairs
 *
 *   It creates a cass statement and if successful sets the caller's
 *   pointer to a pointer to a cass statement to that statement
 *
 *   It returns a standard Tcl result, TCL_ERROR if something went
 *   wrong and you don't get a statement
 *
 *   It returns TCL_OK if all went well
 *
 *   This uses casstcl_make_upsert_statement to make the statement after
 *   it figures the arguments thereto
 *
 * Results:
 *      A standard Tcl result.
 *
 *----------------------------------------------------------------------
 */
int
casstcl_make_upsert_statement_from_objv (casstcl_sessionClientData *ct, int objc, Tcl_Obj *CONST objv[], CassConsistency *consistencyPtr, CassStatement **statementPtr)
{
	Tcl_Interp *interp = ct->interp;
	int ifNotExists = 0;
	int dropUnknown = 0;
	char *mapUnknown = NULL;
	int arg = 0;

    int         optIndex;
    static CONST char *options[] = {
        "-mapunknown",
        "-nocomplain",
        "-ifnotexists",
        NULL
    };

    enum options {
        OPT_MAPUNKNOWN,
        OPT_NOCOMPLAIN,
        OPT_IFNOTEXISTS,
    };

    /* basic validation of command line arguments */
    if (objc < 2) {
        Tcl_WrongNumArgs (interp, 0, objv, "?-mapunknown columnName? ?-nocomplain? ?-ifnotexists? keyspace.tableName keyValuePairList");
        return TCL_ERROR;
    }

	for (arg = 0; arg < objc - 2; arg++) {
		if (Tcl_GetIndexFromObj (interp, objv[arg], options, "option", TCL_EXACT, &optIndex) != TCL_OK) {
			return TCL_ERROR;
		}

		switch ((enum options) optIndex) {
			case OPT_MAPUNKNOWN: {
				if (arg + 1 >= objc - 2) {
					Tcl_ResetResult (interp);
					Tcl_AppendResult (interp, "-mapunknown requires a column name", NULL);
					return TCL_ERROR;
				}

				mapUnknown = Tcl_GetString (objv[++arg]);
				break;
			}

			case OPT_NOCOMPLAIN: {
				dropUnknown = 1;
				break;
			}

			case OPT_IFNOTEXISTS: {
				ifNotExists = 1;
				break;
			}
		}
	}

	char *tableName = Tcl_GetString (objv[objc - 2]);

	return casstcl_make_upsert_statement (ct, tableName, objv[objc - 1], consistencyPtr, statementPtr, mapUnknown, dropUnknown, ifNotExists);
}

/*
 *----------------------------------------------------------------------
 *
 * casstcl_select --
 *
 *      Given a cassandra query, array name and Tcl_Obj pointing to some
 *      Tcl code, perform the select, filling the named array with elements
 *      from each row in turn and executing code against it.
 *
 *      break, continue and return are supported (probably)
 *
 *      Issuing commands with async and processing the results with
 *      async foreach allows for greater concurrency.
 *
 * Results:
 *      A standard Tcl result.
 *
 *
 *----------------------------------------------------------------------
 */

int casstcl_select (casstcl_sessionClientData *ct, char *query, char *arrayName, Tcl_Obj *codeObj, int pagingSize, CassConsistency *consistencyPtr) {
	CassStatement* statement = NULL;
	int tclReturn = TCL_OK;
	Tcl_Interp *interp = ct->interp;

	statement = cass_statement_new(query, 0);

	cass_bool_t has_more_pages = cass_false;
	const CassResult* result = NULL;
	CassError rc = CASS_OK;
	int columnCount = -1;

	if (casstcl_setStatementConsistency(ct, statement, consistencyPtr) != TCL_OK) {
		return TCL_ERROR;
	}

	cass_statement_set_paging_size(statement, pagingSize);

	do {
		CassIterator* iterator;
		CassFuture* future = cass_session_execute(ct->session, statement);

		rc = cass_future_error_code(future);
		if (rc != CASS_OK) {
			tclReturn = casstcl_future_error_to_tcl (ct, rc, future);
			cass_future_free(future);
			break;
		}

		/*
		 * NOTE: *DEFENSIVE PROGRAMMING* This NULL check is probably
		 *       not absolutely required here; however, I discovered
		 *       that it is possible to have a successful future with
		 *       no result.
		 */
		result = cass_future_get_result(future);

		if (result == NULL) {
			Tcl_ResetResult (interp);
			Tcl_AppendResult (interp, "future has no result", NULL);
			tclReturn = TCL_ERROR;
			break;
		}

		iterator = cass_iterator_from_result(result);
		cass_future_free(future);

		if (columnCount == -1) {
			columnCount = cass_result_column_count (result);
		}

		while (cass_iterator_next(iterator)) {
			CassString cassNameString;
			int i;

			const CassRow* row = cass_iterator_get_row(iterator);

			// process all the columns into the tcl array
			for (i = 0; i < columnCount; i++) {
				Tcl_Obj *newObj = NULL;
				const char *columnName;
				const CassValue *columnValue;

				cass_result_column_name (result, i, &cassNameString.data, &cassNameString.length);
				columnName = cassNameString.data;

				columnValue = cass_row_get_column (row, i);

				if (cass_value_is_null (columnValue)) {
					Tcl_UnsetVar2 (interp, arrayName, columnName, 0);
					continue;
				}

				if (casstcl_cass_value_to_tcl_obj (ct, columnValue, &newObj) == TCL_ERROR) {
					tclReturn = TCL_ERROR;
					break;
				}

				if (newObj == NULL) {
					Tcl_UnsetVar2 (interp, arrayName, columnName, 0);
				} else {
					if (Tcl_SetVar2Ex (interp, arrayName, columnName, newObj, (TCL_LEAVE_ERR_MSG)) == NULL) {
						tclReturn = TCL_ERROR;
						break;
					}
				}
			}

			// now execute the code body
			int evalReturnCode = Tcl_EvalObjEx(interp, codeObj, 0);
			if ((evalReturnCode != TCL_OK) && (evalReturnCode != TCL_CONTINUE)) {
				if (evalReturnCode == TCL_BREAK) {
					tclReturn = TCL_BREAK;
				}

				if (evalReturnCode == TCL_ERROR) {
					char        msg[60];

					tclReturn = TCL_ERROR;

					sprintf(msg, "\n    (\"select\" body line %d)",
							Tcl_GetErrorLine(interp));
					Tcl_AddErrorInfo(interp, msg);
				}

				break;
			}
		}

		has_more_pages = cass_result_has_more_pages(result);

		if (has_more_pages) {
			cass_statement_set_paging_state(statement, result);
		}

		cass_iterator_free(iterator);
		cass_result_free(result);
	} while (has_more_pages && tclReturn == TCL_OK);

	cass_statement_free(statement);
	Tcl_UnsetVar (interp, arrayName, 0);

	if (tclReturn == TCL_BREAK) {
		tclReturn = TCL_OK;
	}

	return tclReturn;
}


/*
 *----------------------------------------------------------------------
 *
 * casstcl_list_keyspaces --
 *
 *      Return a list of the extant keyspaces in the cluster by
 *      examining the metadata managed by the driver.
 *
 *      The cpp-driver docs indicate that the driver stays abreast with
 *      changes to the schema so we prefer to ask it rather than
 *      caching our own copy, or something.
 *
 * Results:
 *      A standard Tcl result.
 *
 *----------------------------------------------------------------------
 */
int
casstcl_list_keyspaces (casstcl_sessionClientData *ct, Tcl_Obj **objPtr) {
	const CassSchema *schema = cass_session_get_schema(ct->session);
	CassIterator *iterator = cass_iterator_from_schema(schema);
	Tcl_Obj *listObj = Tcl_NewObj();
	int tclReturn = TCL_OK;

	while (cass_iterator_next(iterator)) {
		CassString name;
		const CassSchemaMeta *schemaMeta = cass_iterator_get_schema_meta (iterator);

		const CassSchemaMetaField* field = cass_schema_meta_get_field(schemaMeta, "keyspace_name");
		cass_value_get_string(cass_schema_meta_field_value(field), &name.data, &name.length);
		if (Tcl_ListObjAppendElement (ct->interp, listObj, Tcl_NewStringObj (name.data, name.length)) == TCL_ERROR) {
			tclReturn = TCL_ERROR;
			break;
		}
	}
	cass_iterator_free(iterator);
	cass_schema_free(schema);
	*objPtr = listObj;
	return tclReturn;
}

/*
 *----------------------------------------------------------------------
 *
 * casstcl_list_tables --
 *
 *      Set the Tcl result to a list of the extant tables in a keyspace by
 *      examining the metadata managed by the driver.
 *
 *      This is cool because the driver will update the metadata if the
 *      schema changes during the session and further examinations of the
 *      metadata by the casstcl metadata-accessing functions will see the
 *      changes
 *
 * Results:
 *      A standard Tcl result.
 *
 *----------------------------------------------------------------------
 */
int
casstcl_list_tables (casstcl_sessionClientData *ct, char *keyspace, Tcl_Obj **objPtr) {
	const CassSchema *schema = cass_session_get_schema(ct->session);
	const CassSchemaMeta *keyspaceMeta = cass_schema_get_keyspace (schema, keyspace);
	Tcl_Interp *interp = ct->interp;

	if (keyspaceMeta == NULL) {
		Tcl_ResetResult (interp);
		Tcl_AppendResult (interp, "keyspace '", keyspace, "' not found", NULL);
		return TCL_ERROR;
	}

	CassIterator *iterator = cass_iterator_from_schema_meta (keyspaceMeta);
	Tcl_Obj *listObj = Tcl_NewObj();
	int tclReturn = TCL_OK;

	while (cass_iterator_next(iterator)) {
		CassString name;
		const CassSchemaMeta *tableMeta = cass_iterator_get_schema_meta (iterator);

		assert (cass_schema_meta_type(tableMeta) == CASS_SCHEMA_META_TYPE_TABLE);

		const CassSchemaMetaField* field = cass_schema_meta_get_field(tableMeta, "columnfamily_name");
		assert (field != NULL);
		cass_value_get_string(cass_schema_meta_field_value(field), &name.data, &name.length);
		if (Tcl_ListObjAppendElement (interp, listObj, Tcl_NewStringObj (name.data, name.length)) == TCL_ERROR) {
			tclReturn = TCL_ERROR;
			break;
		}
	}
	cass_iterator_free(iterator);
	cass_schema_free(schema);
	*objPtr = listObj;
	return tclReturn;
}

/*
 *----------------------------------------------------------------------
 *
 * casstcl_list_columns --
 *
 *      Set a Tcl object pointer to a list of the extant columns in the
 *      specified table in the specified keyspace by examining the
 *      metadata managed by the driver.
 *
 *      If includeTypes is 1 then instead of listing just the columns it
 *      also lists their data types, as a list of key-value pairs.
 *
 * Results:
 *      A standard Tcl result.
 *
 *----------------------------------------------------------------------
 */
int
casstcl_list_columns (casstcl_sessionClientData *ct, char *keyspace, char *table, int includeTypes, Tcl_Obj **objPtr) {
	const CassSchema *schema = cass_session_get_schema(ct->session);
	Tcl_Interp *interp = ct->interp;

	// locate the keyspace
	const CassSchemaMeta *keyspaceMeta = cass_schema_get_keyspace (schema, keyspace);

	if (keyspaceMeta == NULL) {
		Tcl_ResetResult (interp);
		Tcl_AppendResult (interp, "keyspace '", keyspace, "' not found", NULL);
		return TCL_ERROR;
	}

	// locate the table within the keyspace
	const CassSchemaMeta *tableMeta = cass_schema_meta_get_entry (keyspaceMeta, table);

	if (tableMeta == NULL) {
		Tcl_ResetResult (interp);
		Tcl_AppendResult (interp, "table '", table, "' not found in keyspace '", keyspace, "'", NULL);
		return TCL_ERROR;
	}

	// prepare to iterate on the columns within the table
	CassIterator *iterator = cass_iterator_from_schema_meta (tableMeta);
	Tcl_Obj *listObj = Tcl_NewObj();
	int tclReturn = TCL_OK;

	// iterate on the columns within the table
	while (cass_iterator_next(iterator)) {
		CassString name;
		const CassSchemaMeta *columnMeta = cass_iterator_get_schema_meta (iterator);

		assert (cass_schema_meta_type(columnMeta) == CASS_SCHEMA_META_TYPE_COLUMN);

		// get the field name and append it to the list we are creating
		const CassSchemaMetaField* field = cass_schema_meta_get_field(columnMeta, "column_name");
		assert (field != NULL);
		const CassValue *fieldValue = cass_schema_meta_field_value(field);
		CassValueType valueType = cass_value_type (fieldValue);

		// it's a crash if you don't check the data type of valueType
		// there's something fishy in system.IndexInfo, a field that
		// doesn't  have a column name
		if (valueType != CASS_VALUE_TYPE_VARCHAR) {
			continue;
		}
		cass_value_get_string(fieldValue, &name.data, &name.length);
		if (Tcl_ListObjAppendElement (interp, listObj, Tcl_NewStringObj (name.data, name.length)) == TCL_ERROR) {
			tclReturn = TCL_ERROR;
			break;
		}
		// if including types then get the data type and append it to the
		// list too
		if (includeTypes) {
			CassString name;
			const CassSchemaMetaField* field = cass_schema_meta_get_field (columnMeta, "validator");
			assert (field != NULL);

			cass_value_get_string(cass_schema_meta_field_value(field), &name.data, &name.length);

			// check the cache array directly from C to avoid calling
			// Tcl_Eval if possible
			Tcl_Obj *elementObj = Tcl_GetVar2Ex (interp, "::casstcl::validatorTypeLookupCache", name.data, (TCL_GLOBAL_ONLY));

			// not there, gotta call Tcl to do the heavy lifting
			if (elementObj == NULL) {
				Tcl_Obj *evalObjv[2];
				// construct a call to our casstcl.tcl library function
				// validator_to_type to translate the value to a cassandra
				// data type to text/list
				evalObjv[0] = Tcl_NewStringObj ("::casstcl::validator_to_type", -1);

				evalObjv[1] = Tcl_NewStringObj (name.data, name.length);

				Tcl_IncrRefCount (evalObjv[0]);
				Tcl_IncrRefCount (evalObjv[1]);
				tclReturn = Tcl_EvalObjv (interp, 2, evalObjv, (TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT));
				Tcl_DecrRefCount(evalObjv[0]);
				Tcl_DecrRefCount(evalObjv[1]);

				if (tclReturn == TCL_ERROR) {
					goto error;
				}
				tclReturn = TCL_OK;

				elementObj = Tcl_GetObjResult (interp);
				Tcl_IncrRefCount(elementObj);
			}

			// we got here, either we found elementObj by looking it up
			// from the ::casstcl::validatorTypeLookCache array or
			// by invoking eval on ::casstcl::validator_to_type

			if (Tcl_ListObjAppendElement (interp, listObj, elementObj) == TCL_ERROR) {
				tclReturn = TCL_ERROR;
				break;
			}
		}
	}
  error:
	cass_iterator_free(iterator);
	cass_schema_free(schema);
	*objPtr = listObj;

	if (tclReturn == TCL_OK) {
		Tcl_ResetResult (interp);
	}

	return tclReturn;
}


/*
 *----------------------------------------------------------------------
 *
 * casstcl_cassObjCmd --
 *
 *      Create a cass object...
 *
 *      cass create my_cass
 *      cass create #auto
 *
 * The created object is invoked to do things with a CassDB
 *
 * Results:
 *      A standard Tcl result.
 *
 *
 *----------------------------------------------------------------------
 */

    /* ARGSUSED */
int
casstcl_cassObjCmd(ClientData clientData, Tcl_Interp *interp, int objc, Tcl_Obj *CONST objv[])
{
    casstcl_sessionClientData *ct;
    int                 optIndex;
    char               *commandName;
    int                 autoGeneratedName;

    static CONST char *options[] = {
        "create",
        "logging_callback",
        "log_level",
        NULL
    };

    enum options {
        OPT_CREATE,
		OPT_LOGGING_CALLBACK,
		OPT_LOG_LEVEL
    };

    // basic command line processing
    if (objc < 2) {
        Tcl_WrongNumArgs (interp, 1, objv, "subcommand ?args?");
        return TCL_ERROR;
    }

    // argument must be one of the subOptions defined above
    if (Tcl_GetIndexFromObj (interp, objv[1], options, "option",
        TCL_EXACT, &optIndex) != TCL_OK) {
        return TCL_ERROR;
    }

    switch ((enum options) optIndex) {
		case OPT_CREATE: {
			if (objc != 3) {
				Tcl_WrongNumArgs (interp, 1, objv, "option arg");
				return TCL_ERROR;
			}

			// allocate one of our cass client data objects for Tcl and configure it
			ct = (casstcl_sessionClientData *)ckalloc (sizeof (casstcl_sessionClientData));

			ct->cass_session_magic = CASS_SESSION_MAGIC;
			ct->interp = interp;
			ct->session = cass_session_new ();
			ct->cluster = cass_cluster_new ();
			ct->ssl = cass_ssl_new ();

			ct->threadId = Tcl_GetCurrentThread();

			Tcl_CreateEventSource (casstcl_EventSetupProc, casstcl_EventCheckProc, NULL);

			commandName = Tcl_GetString (objv[2]);

			// if commandName is #auto, generate a unique name for the object
			autoGeneratedName = 0;
			if (strcmp (commandName, "#auto") == 0) {
				static unsigned long nextAutoCounter = 0;
				char *objName;
				int    baseNameLength;

				objName = Tcl_GetStringFromObj (objv[0], &baseNameLength);
				baseNameLength += snprintf (NULL, 0, "%lu", nextAutoCounter) + 1;
				commandName = ckalloc (baseNameLength);
				snprintf (commandName, baseNameLength, "%s%lu", objName, nextAutoCounter++);
				autoGeneratedName = 1;
			}

			// create a Tcl command to interface to cass
			ct->cmdToken = Tcl_CreateObjCommand (interp, commandName, casstcl_cassObjectObjCmd, ct, casstcl_cassObjectDelete);
			Tcl_SetObjResult (interp, Tcl_NewStringObj (commandName, -1));
			if (autoGeneratedName == 1) {
				ckfree(commandName);
			}
			break;
		}

		case OPT_LOGGING_CALLBACK: {
			if (objc != 3) {
				Tcl_WrongNumArgs (interp, 1, objv, "option arg");
				return TCL_ERROR;
			}

			// if it already isn't null it was set to something, decrement
			// that object's reference count so it will probably be
			// deleted
			if (casstcl_loggingCallbackObj != NULL) {
				Tcl_DecrRefCount (casstcl_loggingCallbackObj);
				casstcl_loggingCallbackObj = NULL;
			}

			casstcl_loggingCallbackObj = objv[2];
			Tcl_IncrRefCount (casstcl_loggingCallbackObj);

			casstcl_loggingCallbackThreadId = Tcl_GetCurrentThread();

			cass_log_set_callback (casstcl_logging_callback, interp);
			break;
		}
		case OPT_LOG_LEVEL: {
			CassLogLevel cassLogLevel;

			if (objc != 3) {
				Tcl_WrongNumArgs (interp, 2, objv, "level");
				return TCL_ERROR;
			}

			if (casstcl_obj_to_cass_log_level(interp, objv[2], &cassLogLevel) == TCL_OK) {
				cass_log_set_level(cassLogLevel);
			} else {
				return TCL_ERROR;
			}
			break;
		}
	}

    return TCL_OK;
}


/*
 *----------------------------------------------------------------------
 *
 * casstcl_make_upsert_statement --
 *
 *   given a session client data, fully qualified table name, Tcl list
 *   obj and a pointer to a statement pointer,
 *
 *   this baby...
 *
 *     ...generates an upsert statement (in the form of insert but that's
 *     how cassandra rolls)
 *
 *     ...uses casstcl_bind_names_from_list to bind the data elements
 *     to the statement using the right data types
 *
 *   It creates a cassandra statement and sets your pointer to it
 *
 * Results:
 *      A standard Tcl result.
 *
 *----------------------------------------------------------------------
 */
int
casstcl_make_upsert_statement (casstcl_sessionClientData *ct, char *tableName, Tcl_Obj *listObj, CassConsistency *consistencyPtr, CassStatement **statementPtr, char *mapUnknown, int dropUnknown, int ifNotExists) {
	int listObjc;
	Tcl_Obj **listObjv;
	Tcl_Interp *interp = ct->interp;
	int tclReturn = TCL_OK;

	if (Tcl_ListObjGetElements (interp, listObj, &listObjc, &listObjv) == TCL_ERROR) {
		Tcl_AppendResult (interp, " while parsing list of key-value pairs", NULL);
		return TCL_ERROR;
	}

	if (listObjc & 1) {
		Tcl_ResetResult (interp);
		Tcl_AppendResult (interp, "key-value pair list must contain an even number of elements", NULL);
		return TCL_ERROR;
	}

	Tcl_DString ds;
	Tcl_DStringInit (&ds);
	Tcl_DStringAppend (&ds, "INSERT INTO ", -1);
	Tcl_DStringAppend (&ds, tableName, -1);
	Tcl_DStringAppend (&ds, " (", 2);

	int i;
	int nFields = 0;
	int didOne = 0;
	int nUnknownToMap = 0;

	casstcl_cassTypeInfo *typeInfo = (casstcl_cassTypeInfo *)ckalloc (sizeof (casstcl_cassTypeInfo) * (listObjc / 2));

	for (i = 0; i < listObjc; i += 2) {
		int varNameLength;

		tclReturn = casstcl_typename_obj_to_cass_value_types (interp, tableName, listObjv[i], &typeInfo[i/2]);

// printf("casstcl_make_upsert_statement figured out i %d table '%s' from '%s' type info %d, %d, %d\n", i, tableName, Tcl_GetString (listObjv[i]), typeInfo[i/2].cassValueType, typeInfo[i/2].valueSubType1, typeInfo[i/2].valueSubType2);

		if (tclReturn == TCL_ERROR) {
			break;
		}

		// failed to find it?
		if (tclReturn == TCL_CONTINUE) {
			if (dropUnknown) {
				tclReturn = TCL_OK;
				continue;
			}

			// if moving of unrecognized colum-value pairs to a map collection
			// is enabled, skip this column for now but keep count of how
			// many columns we are mapping

			if (mapUnknown != NULL) {
				nUnknownToMap++;
				tclReturn = TCL_OK;
				continue;
			}

			// ok, we weren't told to drop unknown columns and we weren't
			// told to map them, so we've found one and it's an error
			Tcl_ResetResult (interp);
			Tcl_AppendResult (interp, "unknown column '", Tcl_GetString(listObjv[i]), "' in upsert for table '", tableName, "'", NULL);
			tclReturn = TCL_ERROR;
			break;
		}

		char *varName = Tcl_GetStringFromObj (listObjv[i], &varNameLength);

		// prepend a comma unless it's the first one
		if (didOne) {
			Tcl_DStringAppend (&ds, ",", 1);
		}

		Tcl_DStringAppend (&ds, varName, varNameLength);
		nFields++;
		didOne = 1;
	}

	// if we were told to map unknown and we found something unknown,
	// append the name of the map column to the insert
	if (nUnknownToMap > 0) {
		if (didOne) {
			Tcl_DStringAppend (&ds, ",", 1);
		}
		Tcl_DStringAppend (&ds, mapUnknown, -1);
		nFields++;
	}

	// now generate the values part of the insert
	Tcl_DStringAppend (&ds, ") values (", -1);

	for (i = 0; i < nFields; i++) {
		if (i > 0) {
			Tcl_DStringAppend (&ds, ",?", 2);
		} else {
			Tcl_DStringAppend (&ds, "?", 1);
		}
	}

	if (ifNotExists) {
		Tcl_DStringAppend (&ds, ") IF NOT EXISTS", -1);
	} else {
		Tcl_DStringAppend (&ds, ")", -1);
	}

	// if we're good to here, bind the variables corresponding to the insert
	if (tclReturn == TCL_OK) {

		char *query = Tcl_DStringValue (&ds);
// printf("nFields %d, upsert query is '%s'\n", nFields, query);
		CassStatement *statement = cass_statement_new (query, nFields);
		int bindField = 0;

		tclReturn = casstcl_setStatementConsistency(ct, statement, consistencyPtr);

		if (tclReturn != TCL_OK) {
			goto cleanup;
		}

		for (i = 0; i < listObjc; i += 2) {
// printf("casstcl_make_upsert_statement i %d type info %d, %d, %d\n", i, typeInfo[i/2].cassValueType, typeInfo[i/2].valueSubType1, typeInfo[i/2].valueSubType2);
			// skip value if type lookup previously determined unknown
			if (typeInfo[i/2].cassValueType == CASS_VALUE_TYPE_UNKNOWN) {
// printf("skip unknown field '%s', value '%s'\n", Tcl_GetString (listObjv[i]), Tcl_GetString(listObjv[i+1]));
				continue;
			}

			// get the value out of the list
			Tcl_Obj *valueObj = listObjv[i+1];

			assert (bindField < nFields);

// printf("bind field %d, name '%s', value '%s'\n", bindField, Tcl_GetString (listObjv[i]), Tcl_GetString(valueObj));
			tclReturn = casstcl_bind_tcl_obj (ct, statement, NULL, 0, bindField++, &typeInfo[i/2], valueObj);
			if (tclReturn == TCL_ERROR) {
				Tcl_AppendResult (interp, " while constructing upsert statement, while attempting to bind field '", Tcl_GetString (listObjv[i]), "' of type '", casstcl_cass_value_type_to_string (typeInfo[i/2].cassValueType), "', value '", Tcl_GetString (valueObj), "' referencing table '", tableName, "'", NULL);
				break;
			}
		}

		// if mapping of unknown column-value pairs is enabled and there are
		// some, map them now
		if (nUnknownToMap > 0) {
			CassCollection *collection = cass_collection_new (CASS_COLLECTION_TYPE_MAP, nUnknownToMap);

			for (i = 0; i < listObjc; i += 2) {
				// consult our cache of the types,
				// skip value if type lookup previously determined known
				if (typeInfo[i/2].cassValueType != CASS_VALUE_TYPE_UNKNOWN) {
					continue;
				}

				// ok, got one, append the column name to the map
				CassError cassError = casstcl_append_tcl_obj_to_collection (ct, collection, CASS_VALUE_TYPE_TEXT, listObjv[i]);
				// i don't think these can really fail, a text conversion
				if (cassError != CASS_OK) {
					tclReturn = casstcl_cass_error_to_tcl (ct, cassError);
					break;
				}

				// now append the column value to the map
				cassError = casstcl_append_tcl_obj_to_collection (ct, collection, CASS_VALUE_TYPE_TEXT, listObjv[i+1]);
				if (cassError != CASS_OK) {
					tclReturn = casstcl_cass_error_to_tcl (ct, cassError);
					break;
				}
			}
			// bind the map collection of key-value pairs to the statement
			assert (bindField < nFields);
// printf("bound collection position %d nFields %d\n", bindField, nFields);
			CassError cassError = cass_statement_bind_collection (statement, bindField, collection);
			cass_collection_free (collection);
			if (cassError != CASS_OK) {
				tclReturn = casstcl_cass_error_to_tcl (ct, cassError);
			}
		}

		// if everything's OK, set the caller's statement pointer to the
		// statement we made
		if (tclReturn == TCL_OK) {
			*statementPtr = statement;
		}
	}

cleanup:

	// free the insert statement
	Tcl_DStringFree (&ds);

	// free the type info cache
	ckfree(typeInfo);

	return tclReturn;
}

/*
 *----------------------------------------------------------------------
 *
 * casstcl_make_statement_from_objv --
 *
 *   This is a beautiful thing because it will work from the like four
 *   places where we generate statements: exec, select, async, and
 *   batch.
 *
 *   This is like what would be in the option-handling case statements.
 *
 *   We will look at the objc and objv we are given with what's in front
 *   of the command that got invoked stripped off, that is for example
 *   if the command was
 *
 *       $batch add -array row $query column column column
 *
 *       $batch add $query $value $type $value $type
 *
 *   ...we expect to get it from "-array" on, that is, they'll pass us
 *   the address of the objv starting from there and the objc properly
 *   discounting whatever preceded the stuff we handle
 *
 *   We then figure it out and invoke the underlying stuff.
 *
 * Results:
 *      A standard Tcl result.
 *
 *----------------------------------------------------------------------
 */
int
casstcl_make_statement_from_objv (casstcl_sessionClientData *ct, int objc, Tcl_Obj *CONST objv[], int argOffset, CassStatement **statementPtr) {
	int arrayStyle = 0;
	char *arrayName = NULL;
	char *tableName = NULL;
	char *preparedName = NULL;
	char *consistencyName = NULL;
	Tcl_Obj *consistencyObj = NULL;
	CassConsistency consistency;
	Tcl_Interp *interp = ct->interp;

    static CONST char *options[] = {
        "-array",
		"-table",
		"-prepared",
		"-consistency",
        NULL
    };

    enum options {
        OPT_ARRAY,
		OPT_TABLE,
		OPT_PREPARED,
		OPT_CONSISTENCY
	};

	int newObjc = objc - argOffset;
	Tcl_Obj *CONST *newObjv = objv + argOffset;
	int optIndex;
	int arg = 0;

	while (arg < newObjc) {
		char *optionString = Tcl_GetString (newObjv[arg]);

		// if the first character isn't a dash, we're done here.
		// this is going to get called a lot so i don't want
		// Tcl_GetIndexFromObj writing an error message and all
		// that stuff unless there really is an option
		if (*optionString != '-') {
			break;
		}

		// OK so we aren't going to accept anything starting with - that
		// isn't in our option list
		if (Tcl_GetIndexFromObj (interp, newObjv[arg++], options, "option",
			TCL_EXACT, &optIndex) != TCL_OK) {
			return TCL_ERROR;
		}

		switch ((enum options) optIndex) {
			case OPT_ARRAY: {
				if (arg >= newObjc) {
					goto wrong_numargs;
				}

				arrayName = Tcl_GetString (newObjv[arg++]);
				arrayStyle = 1;
				break;
			}

			case OPT_TABLE: {
				if (arg >= newObjc) {
					goto wrong_numargs;
				}

				tableName = Tcl_GetString (newObjv[arg++]);
				arrayStyle = 1;
				break;
			}

			case OPT_PREPARED: {
				if (arg >= newObjc) {
					goto wrong_numargs;
				}

				preparedName = Tcl_GetString (newObjv[arg++]);
// printf("saw prepared case, name = '%s'\n", preparedName);
				break;
			}

			case OPT_CONSISTENCY: {
				if (arg >= newObjc) {
					goto wrong_numargs;
				}

				consistencyObj = newObjv[arg++];
				consistencyName = Tcl_GetString(consistencyObj);
// printf("saw consistency case, name = '%s'\n", consistencyName);

				if (strlen(consistencyName) > 0 && casstcl_obj_to_cass_consistency(ct, consistencyObj, &consistency) != TCL_OK) {
					return TCL_ERROR;
				}
				break;
			}
		}
	}

//printf ("looking for query, arg %d, newObjc %d\n", arg, newObjc);

	// There are several different possibilities here in terms of the list
	// of arguments (i.e. the ones already processed and those that remain
	// to be processed):
	//
	//     1. There are no arguments left.  This is fine if the query is
	//        prepared (i.e. the -prepared option was processed).  This
	//        means there are *NO* name/value pairs to bind.
	//
	//     2. There is exactly one argument left.  This is always fine.
	//
	//     3. There is more than one argument left.  This is fine if the
	//        -prepared option was not processed; otherwise, this is an
	//        error.
	//
	// This check is used to determine if we ran out of arguments without
	// having processed the -prepared option.
	//
	if (arg >= newObjc && preparedName == NULL) {
	  wrong_numargs:
		Tcl_WrongNumArgs (interp, (argOffset <= 2) ? argOffset : 2, objv, "?-array arrayName? ?-table tableName? ?-prepared preparedName? ?-consistency level? ?query? ?arg...?");
		return TCL_ERROR;
	}

	if (preparedName != NULL && arrayStyle) {
		Tcl_ResetResult (interp);
		Tcl_AppendResult (interp, "-prepared cannot be used with -table / -array", NULL);
		return TCL_ERROR;
	}

	// if prepared, handle it
	if (preparedName != NULL) {
// printf("prepared case branch, name = '%s'\n", preparedName);

		// locate the prepared statement structure we created earlier
		casstcl_preparedClientData * pcd = casstcl_prepared_command_to_preparedClientData (interp, preparedName);
		int listObjc = 0;
		Tcl_Obj **listObjv = NULL;

		if (pcd == NULL) {
			Tcl_ResetResult (interp);
			Tcl_AppendResult (interp, "-prepared argument '", preparedName, "' isn't a valid prepared statement object", NULL);
			return TCL_ERROR;
		}

		// there must be exactly one argument left.  the list of
		// name/value pairs, which must contain an even number of
		// elements.
		if (arg < newObjc && arg + 1 != newObjc) {
			Tcl_WrongNumArgs (interp, (argOffset <= 2) ? argOffset : 2, objv, "-prepared prepared ?list?");
			return TCL_ERROR;
		}

		if (arg < newObjc) {
			// split out the column_name-value pairs of names and values
			if (Tcl_ListObjGetElements (interp, newObjv[arg++], &listObjc, &listObjv) == TCL_ERROR) {
				Tcl_AppendResult (interp, " while parsing list of column-value pairs", NULL);
				return TCL_ERROR;
			}

			if (listObjc & 1) {
				Tcl_ResetResult (interp);
				Tcl_AppendResult (interp, "list must contain an even number of elements", NULL);
				return TCL_ERROR;
			}
		}
		return casstcl_bind_names_from_prepared (pcd, listObjc, listObjv, (consistencyObj != NULL) ? &consistency : NULL, statementPtr);
	}

	char *query = Tcl_GetString (newObjv[arg++]);
	// (whatever is left of the newObjv from arg to the end are column-related)

	if (arrayStyle) {
		if (tableName == NULL) {
			Tcl_ResetResult (interp);
			Tcl_AppendResult (interp, "-table must be specified if -array is specified", NULL);
			return TCL_ERROR;
		}

		if (arrayName == NULL) {
			Tcl_ResetResult (interp);
			Tcl_AppendResult (interp, "-array must be specified if -table is specified", NULL);
			return TCL_ERROR;
		}

		return casstcl_bind_names_from_array (ct, tableName, query, arrayName, newObjc - arg, &newObjv[arg], (consistencyObj != NULL) ? &consistency : NULL, statementPtr);
	} else {
		return casstcl_bind_values_and_types (ct, query, newObjc - arg, &newObjv[arg], (consistencyObj != NULL) ? &consistency : NULL, statementPtr);
	}
}



/*
 *----------------------------------------------------------------------
 *
 * casstcl_reimport_column_type_map --
 *    Call out to the Tcl interpreter to invoke
 *    ::casstcl::import_column_type_map from the casstcl library;
 *    the proc resides in source file is casstcl.tcl.
 *
 *    This convenience function gets called from a method of the
 *    casstcl cass object and is invoked upon connection as well
 *
 * Results:
 *    The program compiles.
 *
 *----------------------------------------------------------------------
 */
int
casstcl_reimport_column_type_map (casstcl_sessionClientData *ct)
{
	int tclReturnCode;
	Tcl_Interp *interp = ct->interp;
	Tcl_Obj *evalObjv[2];

	// construct an objv we'll pass to eval.
	// first is the command
	// second is the name of cassandra connection object
	evalObjv[0] = Tcl_NewStringObj ("::casstcl::import_column_type_map", -1);
	evalObjv[1] = Tcl_NewObj();
	Tcl_GetCommandFullName(interp, ct->cmdToken, evalObjv[1]);

	// eval the command.  this should traverse the metadata and extract
	// the types of all the columns of all the tables of all the keyspaces

	Tcl_IncrRefCount (evalObjv[0]);
	Tcl_IncrRefCount (evalObjv[1]);

	tclReturnCode = Tcl_EvalObjv (interp, 2, evalObjv, (TCL_EVAL_GLOBAL|TCL_EVAL_DIRECT));

	Tcl_DecrRefCount(evalObjv[0]);
	Tcl_DecrRefCount(evalObjv[1]);

	return tclReturnCode;
}

/* vim: set ts=4 sw=4 sts=4 noet : */
