-- The RPC returns a column named observation_id. In PL/pgSQL that output
-- parameter made the column-list form of ON CONFLICT ambiguous at runtime.
-- Target the named unique constraint instead, which is unambiguous and keeps
-- the public RPC response contract unchanged.

set lock_timeout = '5s';
set statement_timeout = '30s';

do $migration$
declare
  function_signature constant regprocedure :=
    'public.ingest_tlv_observation(smallint,integer,integer,bigint,smallint,smallint,smallint,smallint,boolean,double precision,double precision,integer,integer,integer,integer,jsonb,text,text,text,text,text,text,integer,bigint,double precision,double precision,double precision,double precision,double precision)'::regprocedure;
  current_definition text;
  corrected_definition text;
  ambiguous_clause constant text :=
    'on conflict (observation_id, route_key) do update';
  corrected_clause constant text :=
    'on conflict on constraint observation_paths_route_unique do update';
begin
  select pg_get_functiondef(function_signature)
  into current_definition;

  if strpos(current_definition, corrected_clause) > 0 then
    return;
  end if;

  if strpos(current_definition, ambiguous_clause) = 0 then
    raise exception 'ingest_tlv_observation does not contain the expected conflict clause';
  end if;

  corrected_definition := replace(
    current_definition,
    ambiguous_clause,
    corrected_clause
  );

  if corrected_definition = current_definition then
    raise exception 'ingest_tlv_observation conflict clause was not updated';
  end if;

  execute corrected_definition;
end
$migration$;
