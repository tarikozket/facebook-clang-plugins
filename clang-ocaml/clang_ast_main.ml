(*
 *  Copyright (c) 2015, Facebook, Inc.
 *  All rights reserved.
 *
 *  This source code is licensed under the BSD-style license found in the
 *  LICENSE file in the root directory of this source tree. An additional grant
 *  of patent rights can be found in the PATENTS file in the same directory.
 *
 *)

module PointerOrd = struct
    type t = Clang_ast_t.pointer
    let compare = String.compare
  end
module PointerMap = Map.Make(PointerOrd)

let declMap = ref PointerMap.empty
let stmtMap = ref PointerMap.empty
let typeMap = ref PointerMap.empty

let empty_v = Clang_ast_visit.empty_visitor
(* This function is not thread-safe *)
let visit_ast ?(visit_decl=empty_v) ?(visit_stmt=empty_v) ?(visit_type=empty_v) top_decl =
	Clang_ast_visit.decl_visitor := visit_decl;
	Clang_ast_visit.stmt_visitor := visit_stmt;
	Clang_ast_visit.type_visitor := visit_type;
	ignore (Clang_ast_v.validate_decl [] top_decl) (* visit *)

let get_ptr_from_node node =
	match node with
	| `DeclNode decl ->
		let decl_info = Clang_ast_proj.get_decl_tuple decl in
		decl_info.Clang_ast_t.di_pointer
	| `StmtNode stmt ->
		let stmt_info,_ = Clang_ast_proj.get_stmt_tuple stmt in
		stmt_info.Clang_ast_t.si_pointer
	| `TypeNode c_type ->
		let type_info = Clang_ast_proj.get_type_tuple c_type in
		type_info.Clang_ast_t.ti_pointer

let get_val_from_node node = 
	match node with
	| `DeclNode decl -> decl
	| `StmtNode stmt -> stmt
	| `TypeNode c_type -> c_type

let add_node_to_cache node cache =
	let key = get_ptr_from_node node in
	let value = get_val_from_node node in
	cache := PointerMap.add key value !cache

let add_decl_to_cache path decl =
	add_node_to_cache (`DeclNode decl) declMap 

let add_stmt_to_cache path stmt =
	add_node_to_cache (`StmtNode stmt) stmtMap 

let add_type_to_cache path c_type = 
	add_node_to_cache (`TypeNode c_type) typeMap 

let reset_cache () =
	declMap := PointerMap.empty;
	stmtMap := PointerMap.empty;
	typeMap := PointerMap.empty

(* This function is not thread-safe *)
let index_node_pointers top_decl =
	(* just in case *)
	reset_cache ();
	(* populate cache *)
	visit_ast ~visit_decl:add_decl_to_cache ~visit_stmt:add_stmt_to_cache ~visit_type:add_type_to_cache top_decl;
	let result = !declMap, !stmtMap, !typeMap in
	reset_cache ();
	result
